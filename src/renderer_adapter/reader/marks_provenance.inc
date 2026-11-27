
// Strip-360 C7: the render-state group taken from the library's own dirty marks. Every library setter that changes a register image marks
// it in the device's main (+16), fetch (+24, bits 32-63: register block 0x2280) or misc (+32) mask, and in owner mode the null draw after
// the reader clears all of them (d3d_layer.h OwnerNullDraw; a clear or resolve ORs its bits back, owner_null_copy.h). So at a draw's entry
// "no mark" means no setter ran since the last draw. One exception: the shader bind the null draw runs when main-mask bits 17-20 are set
// writes register images the reader gathers (+10528 / +10532 / +10536 / +10560 / +10580, d3d_shader_bind.h Commit) AFTER the reader read
// them, so the draw after a bind always reads the device. `--wwe2k14_rdr_debug_marks_verify` reads it anyway and counts misses.
// Read at the draw's flush point, before the null draw: the indexed draw's entry, the inline draw's Begin (its flush is at Begin, the
// reader reads it at End: End adds that nothing was marked in between).
inline bool g_prev_draw_bound = true;
inline bool MarksOn() { return REXCVAR_GET(wwe2k14_rdr_owner) && REXCVAR_GET(wwe2k14_rdr_state_by_marks); }
inline bool NoMarks(const uint8_t* base, uint32_t dev) {
  return !db::Ld64(base, dev + 16) && !(db::Ld64(base, dev + 24) >> 32) && !db::Ld64(base, dev + 32);
}
// forced_bind: the inline Begin's stride rule (a stride other than the bound one raises bit 19, d3d_layer_inline.h OwnerNullBegin).
struct MarksDbg {  // S102 T18 (diagnosis): the raw masks ReadMarks saw at the last draw entry (kept only while wwe2k14_rdr_debug_particle_marks > 0)
  uint64_t main = 0, fetch = 0, misc = 0;
  bool prev_bound = false, forced = false;
};
inline MarksDbg g_marks_dbg;
inline bool ReadMarks(const uint8_t* base, uint32_t dev, bool forced_bind = false) {
  if (REXCVAR_GET(wwe2k14_rdr_debug_particle_marks) > 0) g_marks_dbg = {db::Ld64(base, dev + 16), db::Ld64(base, dev + 24), db::Ld64(base, dev + 32), g_prev_draw_bound, forced_bind};
  rdr_adapter::particle_lite::NoteEntry();  // S102 T18 (C7 hole): one count per draw entry, recorded or not (a draw that returns early records nothing, its setters' marks are cleared all the same)
  const bool unmarked = !g_prev_draw_bound && NoMarks(base, dev);
  g_prev_draw_bound = forced_bind || (uint32_t(db::Ld64(base, dev + 16)) & 0x1E0000u) != 0;
  return unmarked;
}

// S101 names (docs\renderer\NAMES.md): the frame's string table and each draw's origin, from the tag its record carried.
// `s` must be a literal or otherwise live for the frame (the map keys on the pointer).
inline uint32_t FrameString(FrameTables& t, const char* s) { return rdr_adapter::names_draw::InternName(t.strings, t.string_map, s); }
// S101 names A1c (texture provenance, docs\renderer\NAMES.md): who wrote each 4 KB page last (host_write.h writers), so a texture's newest
// bytes have a declared owner. Written by the host writers (any thread), read at frame serialisation. Paths kept in a deque: stable c_str.
// (PageWriter / ProvenanceState / g_prov are defined with the names counters, above CensusMaybe.)
// S101 lane17: the page map is prov_map.h's flat lock-free array (wwe2k14::prov::Map()); this is only the hook and the label's string/table glue.
inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source) {
  // S101 lane17 (Check C): recorded from app start when provenance is on (_startup.pac opens at frame 0, the live mirror starts ~frame 55: a copy
  // out of a read nobody listened to finds a source with no owner), else only while a capture runs.
  if (!wwe2k14::prov::ShouldRecord(g_prov_on.load(std::memory_order_relaxed), Recording())) return;
  wwe2k14::prov::Map().Note(addr, length, writer, file, source, uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)));
}
// The provenance of one texture image as this frame binds it: the reader's own flags first (declared facts), then its base page's writer.
// `detail` (census only): the writer with what a memcpy's source was.
inline cap::TextureProvenance ProvenanceOf(FrameTables& t, uint32_t id, const char** detail = nullptr) {
  cap::TextureProvenance p;
  p.image = id;
  const Image& img = ById(id);
  wwe2k14::prov::ImageFacts f;
  f.packed = img.packed; f.copy_dest = img.copy_dest; f.flush_driven = img.flush_driven; f.flushed = img.flushed;
  f.flush_seen = img.flush_seen; f.flush_frame = img.flush_frame; f.flush_seq = img.flush_seq; f.first_read_seq = img.first_read_seq; f.copy_source = img.copy_source;
  const wwe2k14::prov::Entry e = wwe2k14::prov::Map().LoadPhysical(img.header[1] & 0xFFFFF000u);
  const wwe2k14::prov::Label l = wwe2k14::prov::Classify(f, e);
  if (detail) *detail = l.detail;
  p.source = l.source;
  p.flags = l.flags;
  if (l.writer) p.writer = FrameString(t, l.writer);
  if (l.file) p.file = FrameString(t, wwe2k14::prov::Map().FileName(l.file));
  p.source_image = l.source_image;
  p.last_write_frame = l.last_write_frame;
  return p;
}
inline void FrameProvenance(FrameTables& t) {
  t.provenance.clear();
  if (!g_prov_on.load(std::memory_order_relaxed) && !g_cap.active) return;  // S101 lane17 review: players pay nothing (no page map was kept)
  std::set<uint32_t> seen;
  for (const cap::TextureSlot& s : t.texture_slots) {
    if (!s.image || !seen.insert(s.image).second) continue;
    if (s.image > g_reg.images.size() || !ById(s.image).is_texture) continue;
    const char* detail = nullptr;
    t.provenance.push_back(ProvenanceOf(t, s.image, &detail));
    std::lock_guard lock(g_prov.mu);
    ++g_prov.by_kind[uint8_t(t.provenance.back().source)];
    const cap::TextureProvenance& pv = t.provenance.back();
    if (pv.source == cap::Provenance::Cpu || pv.source == cap::Provenance::Gpu)  // S101 lane17: (the writer's string: 1-based offset in the frame's table)
      ++g_prov.by_writer[detail ? std::string(detail) : pv.writer ? std::string(t.strings.c_str() + (pv.writer - 1)) : std::string(pv.source == cap::Provenance::Gpu ? "gpu_copy" : "?")];
  }
}

// S101 U1: the draw being taken is t.draws.back() (callers push the draw first); a glyph draw's whole run string goes in kSecTextRuns.
inline cap::DrawOrigin TakeDrawOrigin(FrameTables& t) {
  cap::TextRun run;
  const cap::DrawOrigin o = rdr_adapter::names_draw::TakeDrawOriginInto(t.strings, t.string_map, &run);
  if (run.string && !t.draws.empty()) {
    run.draw = uint32_t(t.draws.size() - 1);
    t.text_runs.push_back(run);
  }
  return o;
}

// lane 122 (D1 thin draws, thin_draw.h; lane 129: any class, the pin set holds the triples of every class that is on): this model-part draw of a pinned key goes lite = the reader builds none of its geometry (the shader lookups, banks and slots stay: they are
// the stream draw's own facts the binder draws with). The key is the draw's origin tag (object, part, pass site); a draw counts toward its key's ordinal whether or not it goes lite,
// exactly like the binder's ComputeKeys over the frame's draw table (the class test below is TakeDrawOriginInto's: submitter / pass row -> ModelPart).
inline void ThinLiteBegin(FrameTables& t) {   // lane 289: factored out (a ring marker can be the first thing of a frame part that reaches the lite counters)
  if (t.lite_begun) return;
  t.lite.SetProofByObject(rdr_adapter::d3::g_arena_d3);  // lane 138 (D3): an arena object's keys take the full path together, in the OBJECT's proof frame
  t.lite.SetWrestlerProofByObject(rdr_adapter::d3::g_wrestlers_d3);  // lane 245 (18.7 precondition 4): the same for a wrestler object's keys (all parts of a marked wrestler are full together in the object's proof frame); the offline emulation does it too (binder.h)
  t.lite.Begin(rdr_adapter::thin::g_pins.Acquire(), wwe2k14::g_guest_frame.load(std::memory_order_relaxed));   // the newest pin set the binder published, read once per frame part (its generation is stamped on the frame)
  t.lite_begun = true;
}
inline bool ThinLiteDraw(FrameTables& t, bool inline_vertices, uint64_t vs_hash, uint64_t ps_hash, const cap::ThinDrawFacts& facts, bool* noquery = nullptr) {   // S102 T18: *noquery = the call returned before Query (nothing counted)
  namespace og = rdr_adapter::origin;
  ThinLiteBegin(t);
  if (noquery) *noquery = true;  // S102 T18: every return false below, up to Query, leaves it set
  if (!t.lite.active()) return false;  // review 7: no pins yet (or none left): nothing can be lite, nothing needs counting, and no origin is resolved
  og::Key k;
  if (!og::Global().Resolve(og::t_replay_tag, k) || k.op != og::t_replay_op) return false;
  if (!k.object || (k.pass_site != rdr_adapter::thin::kSiteColour && k.pass_site != rdr_adapter::thin::kSiteMirror)) return false;
  const og::FnRow* sub = og::FindRow(og::kSubmitters, k.call_site);
  const bool ui_submitter = sub && (sub->klass == cap::DrawClass::Sprite || sub->klass == cap::DrawClass::Movie || sub->klass == cap::DrawClass::Text);
  const og::FnRow* pass = ui_submitter ? nullptr : og::FindRow(og::kPasses, k.pass_site);
  const og::FnRow* row = pass ? pass : sub;
  if (!row || row->klass != cap::DrawClass::ModelPart) return false;
  if (noquery) *noquery = false;  // S102 T18: Query counts
  return t.lite.Query(k.pass_site, k.object, k.part, !inline_vertices, rdr_adapter::thin::ProgramKey(vs_hash, ps_hash, facts));
}

// S102 T18 (diagnosis, wwe2k14_rdr_debug_particle_marks = N): the first N marked candidates' raw marks. The main mask's bits 17-20 are the shader bind (d3d_shader_bind.h Commit); with prev_bound set the draw
// reads the device whatever the masks say (ReadMarks). The other bits are named by the library setter that sets them (d3d_layer*.h): the log prints the numbers, the names come from reading them.
inline void PlLogMarks(const rdr_adapter::particle_lite::Key& k) {
  static int logged = 0;
  const int32_t want = REXCVAR_GET(wwe2k14_rdr_debug_particle_marks);
  if (want <= 0 || logged >= want) return;
  ++logged;
  const MarksDbg& m = g_marks_dbg;
  REXLOG_WARN("[rdr][particle_lite] marks #{} site {:08X}: prev_draw_bound {}, forced_bind {}, main(+16) {:016X} [shader bind bits 17-20: {:X}], fetch(+24) low {:08X} (texture slots) high {:08X} (reg block 0x2280), misc(+32) {:016X}", logged, k.site,
              m.prev_bound, m.forced, m.main, uint32_t(m.main >> 17) & 0xF, uint32_t(m.fetch), uint32_t(m.fetch >> 32), m.misc);
}

// S102 T18 (F1): the slot facts the lite path's copied records are a function of: the GPU's copy of the record (g_gpu_fetch: where the LOW word of dev+24, the texture dirty bits, lands) and
// its object, the object's header bytes, the image's flags, and the registry epoch (the same inputs as AddTextureSlot's SlotKey, plus the flags it re-checks).
inline void PlSnapOne(uint8_t* base, uint32_t dev, uint32_t slot, uint32_t image, rdr_adapter::particle_lite::SlotSnap& o) {
  o.slot = uint8_t(slot);
  o.image = image;
  o.flags = 0;
  std::memset(o.pack_host, 0, sizeof(o.pack_host));
  uint32_t object = 0;
  if (slot < g_gpu_fetch.size() && g_gpu_fetch[slot].known && g_gpu_fetch_device == dev) {
    std::memcpy(o.w, g_gpu_fetch[slot].words, sizeof(o.w));
    object = g_gpu_fetch[slot].object;
  } else {
    for (uint32_t k = 0; k < 6; ++k) o.w[k] = db::Ld32(base, dev + kDevFetch + 24 * slot + 4 * k);
    object = db::Ld32(base, dev + kDevTexObj + 4 * slot);
  }
  o.object = object;
  if (object) std::memcpy(o.header, db::Host(base, object + 28), sizeof(o.header));
  else std::memset(o.header, 0, sizeof(o.header));
  if (image) {
    const Image& img = ById(image);
    o.flags = uint8_t((img.packed ? 1 : 0) | (img.copy_dest ? 2 : 0) | (img.swap ? 4 : 0) | (img.target16 ? 8 : 0));
    std::memcpy(o.pack_host, img.pack_host, sizeof(o.pack_host));
  }
}
inline void PlSnapSlots(uint8_t* base, uint32_t dev, const cap::TextureSlot* recs, uint32_t count, rdr_adapter::particle_lite::SlotsSnap& out) {
  out.epoch = g_reg.epoch;
  out.n = count < 32 ? count : 32;
  for (uint32_t i = 0; i < out.n; ++i) PlSnapOne(base, dev, recs[i].slot, recs[i].image, out.s[i]);
}
// The live state of the slots the previous draw used, against its snapshot.
inline bool PlSlotsSame(uint8_t* base, uint32_t dev, const rdr_adapter::particle_lite::SlotsSnap& prev) {
  rdr_adapter::particle_lite::SlotsSnap now;
  now.epoch = g_reg.epoch;
  now.n = prev.n;
  for (uint32_t i = 0; i < prev.n; ++i) PlSnapOne(base, dev, prev.s[i].slot, prev.s[i].image, now.s[i]);
  return now.Equal(prev);
}
