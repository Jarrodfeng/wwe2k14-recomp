
// ---- the frame being built --------------------------------------------------------------------------------------------------------
struct FrameTables {
  std::vector<cap::Op> ops;
  std::vector<rdr::TargetsOp> targets;
  std::vector<rdr::ClearOp> clears;
  std::vector<cap::Draw> draws;
  std::vector<rdr::CopyOp> copies;
  std::vector<rdr::PresentOp> presents;
  std::vector<cap::DrawFixed> fixeds;
  std::vector<cap::DrawDynamic> dynamics;
  std::vector<rdr::SamplerDesc> samplers;
  std::vector<cap::TextureSlot> texture_slots;
  std::vector<cap::VertexLayout> layouts;
  std::vector<cap::VertexElement> elements;
  std::vector<cap::ConstantBank> constant_banks;
  std::vector<cap::BoolLoop> bool_loops;
  std::vector<cap::ContentRef> contents;
  std::vector<cap::CopyExtra> copy_extras;  // parallel to copies
  std::vector<cap::BankDef> bank_defs;       // draws index these (S100 speed: banks as changes)
  std::vector<cap::BankPatch> bank_patches;
  std::vector<cap::DrawOrigin> origins;      // S101 names: parallel to draws (names.h kSecDrawOrigins)
  std::vector<cap::TextRun> text_runs;       // S101 U1: glyph draws' whole strings (kSecTextRuns)
  rdr_adapter::thin::FrameLite lite;         // lane 122 (D1 thin draws): this frame part's lite decisions and ordinals
  bool lite_begun = false;                   // lane 122: Begin() ran for this part (the pin set was read)
  std::vector<cap::ThinDrawFacts> thin_facts;  // lane 122 (review 6): parallel to draws while the thin switch is on
  std::vector<cap::TextureProvenance> provenance;  // S101 names A1c: the textures this frame binds (kSecProvenance), built at serialisation
  std::string strings;                       // S101 names: kSecStrings; a name field = offset + 1
  std::unordered_map<const char*, uint32_t> string_map;  // keyed by pointer: the names are literals (origin_table.h), no allocation a draw
  // de-duplication, per frame
  std::unordered_map<uint64_t, uint32_t> fixed_map, dynamic_map, sampler_map, layout_map, bank_map, bool_map;
  uint32_t last_targets[5] = {};
  bool targets_valid = false;
  uint64_t generation = 0;  // S100 speed: counts resets; a cached table index is good only within its generation
  void Reset() {
    ++generation;
    ++g_shader_repeat_epoch;  // (the last-shader repeats: within a frame part)
    ops.clear(); targets.clear(); clears.clear(); draws.clear(); copies.clear(); presents.clear(); fixeds.clear(); dynamics.clear(); samplers.clear();
    texture_slots.clear(); layouts.clear(); elements.clear(); constant_banks.clear(); bool_loops.clear(); contents.clear(); copy_extras.clear();
    bank_defs.clear(); bank_patches.clear();
    origins.clear(); text_runs.clear(); provenance.clear(); strings.clear(); string_map.clear();
    fixed_map.clear(); dynamic_map.clear(); sampler_map.clear(); layout_map.clear(); bank_map.clear(); bool_map.clear();
    targets_valid = false;
    lite_begun = false;  // lane 122
    thin_facts.clear();
  }
};
inline FrameTables g_frame;

template <class T>
inline uint32_t Dedupe(std::vector<T>& table, std::unordered_map<uint64_t, uint32_t>& map, const T& value, const uint8_t* compare_bytes, size_t compare_size) {
  const uint64_t h = db::Hash64(compare_bytes, compare_size);
  if (auto it = map.find(h); it != map.end()) {
    if (std::memcmp(&table[it->second], &value, sizeof(T)) == 0) return it->second;
  }
  const uint32_t at = uint32_t(table.size());
  table.push_back(value);
  map[h] = at;
  return at;
}

// ---- the capture session ------------------------------------------------------------------------------------------------------------
struct GeoRef {
  uint64_t v_off = 0, i_off = 0;
  uint32_t v_bytes = 0, i_bytes = 0, v_count = 0, i_count = 0;
  uint8_t idx32 = 0, flags = 0;
};

struct Session {
  cap::CaptureWriter writer;
  bool active = false;
  bool stop = false;
  bool over_logged = false;
  uint32_t frames_left = 0, frame_index = 0;
  std::filesystem::path folder;
  std::unordered_map<uint64_t, GeoRef> geo;
  std::unique_ptr<rdr_scene::live::FolderSink> scene_sink;  // S101 lane 26: attached while a capture runs with --wwe2k14_scene_records
};
inline Session g_cap;
// S101 lane 26: the writer's census and refusal lines go to the run log.
inline const bool g_scene_log_set = (rdr_scene::live::SetLog(+[](const char* line) { REXLOG_INFO("{}", line); }), true);

// ---- the live sink (LIVE_ADAPTER.md): the same frames, drawn by the new renderer instead of written to disk (g_live: top of the file) ---
inline uint64_t g_record_frame = 0;       // frames recorded (capture or live): EnsureContent reads an image at most once per frame
inline std::unordered_map<uint64_t, uint64_t> g_geo_used;  // live: geometry key -> last recorded frame that used it (keys unused for long are dropped)
inline rdr_adapter::geoforget::Expiry g_geo_expiry;  // lane 266: the same keys in store order; LiveForgetGeometry looks only at its head, a little each frame
inline constexpr uint64_t kLiveGeometryKeepFrames = 600;
// Lane 273 (runs 7189 / 7190): an entrance makes ~600 new geometry keys a frame, ~370k live in the keep window. A std::unordered_map rehashes every
// node at the insert that crosses a power of two: one inline build took 12-80 ms "in store" on the render thread. The live maps are sized for the
// steady state once, at live start (buckets stay through clear / erase), so no insert rehashes; tools\perf\geo_map_bench.cpp: worst insert
// 16-21 ms -> 0.04-0.11 ms at 450k keys.
inline constexpr size_t kGeoReserve = size_t(1) << 20;
// S100 speed: bumped whenever stored geometry can be forgotten (LiveForgetGeometry erasing, a capture or live start): a memo entry found
// stored at the current epoch is still stored, without a lookup. Its used mark is refreshed every kGeometryMarkEvery frames (half the
// keep window, so geometry in use never ages out).
inline uint64_t g_geo_forget_epoch = 1;
// lane 266: bumped only where the stores are cleared whole (StartLive / StartCapture). g_geo_forget_epoch also moves when the age queue forgets
// anything (nearly every frame), so a memo entry is also stored when its mark is younger than the keep window (see GeoStillStored).
inline uint64_t g_geo_clear_epoch = 1;
inline constexpr uint64_t kGeometryMarkEvery = kLiveGeometryKeepFrames / 2;

// Capture or live: the heavy work (constants, geometry, texture contents) runs only then.
inline bool Recording() { return g_cap.active || g_live.active(); }
// S102 lane KO (knockouts.h): the gate every reader hook sits behind: --wwe2k14_rdr_capture, and not the reader_none knock-out.
inline bool ReaderOn() { return REXCVAR_GET(wwe2k14_rdr_capture) && !ko::g_reader_none.load(std::memory_order_relaxed); }
// S102 lane KO: called by the present hook AFTER the reader's own OnPresent (a frame is never cut in the middle): takes the switch for the next frame.
inline void KoLatchAtPresent() {
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  const bool now = rdr_sw::BoolSeen(rdr_sw::kKoReaderNone, false, frame);
  if (now && !ko::g_reader_none.load(std::memory_order_relaxed)) ko::KoOnce(2, "wwe2k14_rdr_ko_reader_none", [](const char* n) { REXLOG_INFO("[ko] {} on", n); });
  ko::g_reader_none.store(now, std::memory_order_relaxed);
  if (now) rdr_adapter::origin::g_on.store(false, std::memory_order_relaxed);  // S102 KO review F3: origin tagging is set only in OnPresent, which reader_none skips
}
inline uint64_t SinkAppend(cap::Stream stream, std::vector<uint8_t>&& bytes) {
  ScopedNs timer(g_times.sink);  // lane 273: named in the slow-frame line and the 600-frame line, so a stall in the hand-over is read, not guessed
  ++g_times.sink_n;
  return g_cap.active ? g_cap.writer.Append(stream, std::move(bytes)) : g_live.Append(stream, std::move(bytes));
}

inline void StopCapture(const char* why) {
  if (g_cap.scene_sink) {  // S101 lane 26: detach first (RemoveSink returns once no EndFrame holds it), then drain to disk
    rdr_scene::live::RemoveSink(g_cap.scene_sink.get());
    g_cap.scene_sink->End();
    REXLOG_INFO("[scene][live] capture scene records: written {}, dropped {}, failed {} in {}", g_cap.scene_sink->written(), g_cap.scene_sink->dropped(),
                g_cap.scene_sink->failed(), g_cap.scene_sink->folder());
    g_cap.scene_sink.reset();
  }
  g_cap.writer.End();
  g_cap.active = false;
  REXLOG_INFO("[rdr][capture] done: {} frames in {} ({} MB) - {}", g_cap.frame_index, g_cap.folder.string(), g_cap.writer.bytes_used() >> 20, why);
}
inline void CheckWriter() {
  if (std::string e = g_cap.writer.TakeError(); !e.empty()) {
    REXLOG_ERROR("[rdr][capture] write failed: {}", e);
    g_cap.stop = true;
  }
  if (g_cap.writer.over_budget() && !g_cap.over_logged) {
    g_cap.over_logged = true;
    g_cap.stop = true;
    REXLOG_WARN("[rdr][capture] disk budget reached ({} MB): the capture stops here", g_cap.writer.budget() >> 20);
  }
}

template <class T>
inline void PutBytes(std::vector<uint8_t>& out, const T* data, size_t count) {
  if (!count) return;
  const size_t at = out.size();
  out.resize(at + count * sizeof(T));
  std::memcpy(out.data() + at, data, count * sizeof(T));
}

inline void WriteImagesTable() {
  std::vector<uint8_t> bytes;
  cap::ImageTableHeader h;
  h.count = uint32_t(g_reg.images.size());
  PutBytes(bytes, &h, 1);
  bytes.reserve(sizeof(h) + h.count * sizeof(cap::ImageRecord));
  for (const Image& img : g_reg.images) PutBytes(bytes, &img.rec, 1);
  g_cap.writer.WriteFile("images.bin", std::move(bytes));
}

inline std::vector<uint8_t> FrameBytes(uint64_t guest_frame, uint32_t present_interval, uint32_t index);

inline void WriteFrameFile(uint64_t guest_frame, uint32_t present_interval) {
  char name[64];
  std::snprintf(name, sizeof(name), "frames/frame_%06u.bin", g_cap.frame_index);
  g_cap.writer.WriteFile(name, FrameBytes(guest_frame, present_interval, g_cap.frame_index));
}

// Live: the image table goes to the sink when it grew or a record changed. A changed record (usage gained later, kUsageMutable) is recreated on
// the render side, so its contents are sent again.
inline std::vector<cap::ImageRecord> g_live_sent;
inline void LiveSendImages() {
  // S102 lane G4: per frame. Review F4: off while the game-side bake skip is in Skip mode (it leaves the destinations stale by design: two cuts of one pass).
  rdr_adapter::dead_work::g_skip_card_bake.store(REXCVAR_GET(wwe2k14_rdr_skip_match_card_bake) && REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) < 2, std::memory_order_relaxed);
  rdr_adapter::copy_alias::g_small_bake_off.store(REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) >= 2, std::memory_order_relaxed);   // S102 G12 review F2: the same S1 test as copy_alias_classes, per frame
  rdr_adapter::dead_work::g_skip_prelight.store(REXCVAR_GET(wwe2k14_rdr_skip_dead_prelight), std::memory_order_relaxed);   // (census only, see dead_work.h)
  const bool grew = g_reg.images.size() != g_live_sent.size();
  const bool marked = g_images_dirty;
  if (!marked && !grew && g_record_frame % 60) return;  // S100 speed: nothing written since the last send
  g_images_dirty = false;
  bool changed = grew;
  uint64_t found = 0;
  for (size_t i = 0; i < g_reg.images.size() && i < g_live_sent.size(); ++i) {
    Image& img = g_reg.images[i];
    if (std::memcmp(&img.rec, &g_live_sent[i], sizeof(cap::ImageRecord)) == 0) continue;
    changed = true;
    ++found;
    img.content_written = false;
    img.checked_frame = ~0ull;
  }
  if (found && !marked) NoteCounted("image_record_change_unmarked", found);
  if (!changed) return;
  g_live_sent.resize(g_reg.images.size());
  for (size_t i = 0; i < g_reg.images.size(); ++i) g_live_sent[i] = g_reg.images[i].rec;
  g_live.SetImages(g_live_sent.data(), g_live_sent.size());
}

// Live: geometry keys unused for kLiveGeometryKeepFrames frames are forgotten here and in the sink. Lane 266: a little every frame (the
// age queue's head, a count and a time budget), not everything at record frame 600k: that one present took 210-597 ms in an entrance
// (SKIP_GAME_WORK_MAP.md 19.6). The epoch moves once for a frame that forgot anything (a memo entry must not believe a forgotten key stored).
// A memo entry's "still stored" shortcut (pure: geo_forget.h's test checks it never returns an erased key): the stores were not cleared whole
// since, and either nothing was forgotten since it was found stored, or its used mark is younger than the keep window (the age queue erases
// only when now - used > keep, and the mark is never older than marked_frame).
inline bool GeoStillStored(uint64_t entry_clear, uint64_t entry_stored, uint64_t marked_frame, uint64_t frame, bool live) {
  return rdr_adapter::geoforget::StillStored(entry_clear, g_geo_clear_epoch, entry_stored, g_geo_forget_epoch, marked_frame, frame, kLiveGeometryKeepFrames, live);
}
inline constexpr int64_t kForgetBudgetNs = 1'500'000;  // a frame's hard cap; the backlog (if any) is printed in the [rdr][periodic] line
inline void LiveForgetGeometry() {
  const auto t0 = std::chrono::steady_clock::now();
  const rdr_adapter::geoforget::SweepResult r = g_geo_expiry.Sweep(g_geo_used, g_record_frame, kLiveGeometryKeepFrames, g_geo_expiry.DefaultItems(), kForgetBudgetNs, [](uint64_t key) {
    if (auto geo = g_cap.geo.find(key); geo != g_cap.geo.end()) {
      if (geo->second.v_bytes) g_live.DropGeometry(geo->second.v_off);
      if (geo->second.i_bytes) g_live.DropGeometry(geo->second.i_off);
      g_cap.geo.erase(geo);
    }
  });
  if (r.erased) ++g_geo_forget_epoch;
  g_periodic.forget.Add(PeriodicMs(t0));
  g_periodic.forget_erased += r.erased, g_periodic.forget_looked += r.looked, g_periodic.forget_requeued += r.requeued;
  g_periodic.forget_time_capped += r.time_capped, g_periodic.forget_count_capped += r.count_capped;
  g_periodic.queue_depth = g_geo_expiry.Size();
}

inline void JoinReaderWork();  // S100 speed: the reader workers' results into this frame (defined after EnsureContent)
inline void FrameProvenance(FrameTables& t);  // S101 names A1c (defined with TakeDrawOrigin)
// S102 lane T8: every table of g_frame <-> a FrameData (a swap: the capacity travels, nothing is copied). Symmetric: the same call puts them back.
inline void SwapFrameTables(FrameTables& t, rdr_adapter::handoff::FrameData& d) { rdr_adapter::handoff::SwapTables(t, d); }   //   // (member list: frame_handoff.h WWE2K14_HANDOFF_TABLES; handoff_unit.py lints it against FrameTables)
// The frame (part) as it leaves the reader: the header and the little sections are made here, the tables move into `d` (g_frame is left with `d`'s old, empty
// tables: the caller either swaps them back (FrameBytes) or goes on to Reset, which only clears).
inline void FillFrameData(uint64_t guest_frame, uint32_t present_interval, rdr_adapter::handoff::FrameData& d, uint32_t index) {
  JoinReaderWork();  // every frame (and frame part) is handed over here: its texture contents must be in it
  FrameTables& t = g_frame;
  FrameProvenance(t);  // S101 names A1c: the provenance of every texture this frame (part) binds
  cap::FrameFileHeader h;
  h.capture_frame = index;
  h.flags |= cap::kFrameFetchOrderOriginal | cap::kFrameSixteenPairsHighFirst;
  if (g_jitter.on && (g_jitter.legacy || g_jitter.armed)) h.flags |= cap::kFrameDlssJitter | (g_jitter.phase << 24);  // S10b (lane 185: only a frame the jitter moved; a frame without it carries no jitter for DLSS)
  if (rdr_adapter::stage_view::StampFrame()) h.flags |= cap::kFrameStagePlaying;  // S101 lane 59: this frame was captured while a `!scene play` was playing
  h.guest_frame = guest_frame;
  if (t.lite_begun && t.lite.lite_draws()) h.flags |= cap::kFrameHasThin;  // lane 122 (review 5): a capture / case zip of this frame holds thin draws
  h.present_interval = present_interval;
  cap::D3FrameCounts d3_counts;  // lane 138: the per-frame marker / dropped counts (the section is present only while the D3 switch is on)
  if (rdr_adapter::d3::g_arena_d3) {
    const rdr_adapter::d3::FrameCounts fc = rdr_adapter::d3::g_tally.TakeAndReset();
    d3_counts.markers = fc.markers, d3_counts.dropped = fc.dropped, d3_counts.frame = guest_frame, d3_counts.generation = rdr_adapter::d3::g_marks.generation();
    static_assert(uint8_t(rdr_adapter::d3::Why::Count) <= sizeof(cap::D3FrameCounts::why) / sizeof(uint32_t), "D3FrameCounts::why must hold every Why (review fix 18)");
    for (uint32_t i = 0; i < uint32_t(rdr_adapter::d3::Why::Count); ++i) d3_counts.why[i] = fc.why[i];
  }
  cap::ThinFrameStamp thin_stamp;  // lane 122: the pin-set generation this frame part's lite decisions used (the section is present only when the reader read a pin set)
  if (t.lite_begun) thin_stamp = cap::ThinFrameStamp{t.lite.stamp(), 0, guest_frame, t.lite.lite_draws(), 0};
  d.Clear();   // (a pooled FrameData comes back cleared; a stack one is empty)
  d.h = h;
  d.has_thin = t.lite_begun, d.thin_stamp = thin_stamp;
  d.has_d3 = rdr_adapter::d3::g_arena_d3, d.d3_counts = d3_counts;
  SwapFrameTables(t, d);   // the section list (frame_handoff.h ForEachSection) is the one place that names every section
}

// The bytes of the frame (part): a capture file, the case ring, a read-back part, or a live frame with the hand-over switch off. g_frame keeps its tables.
inline std::vector<uint8_t> FrameBytes(uint64_t guest_frame, uint32_t present_interval, uint32_t index) {
  rdr_adapter::handoff::FrameData d;
  FillFrameData(guest_frame, present_interval, d, index);
  std::vector<uint8_t> file = rdr_adapter::handoff::Serialize(d);
  SwapFrameTables(g_frame, d);   // back (the caller may read g_frame again, or Reset it)
  return file;
}

// S102 lane T8: the frame as a FrameData, handed to the sink without serialising. g_frame is left empty-handed (the pool's cleared tables): Reset follows.
inline std::shared_ptr<rdr_adapter::handoff::FrameData> FrameHandoff(uint64_t guest_frame, uint32_t present_interval, uint32_t index) {
  std::shared_ptr<rdr_adapter::handoff::FrameData> d = rdr_adapter::handoff::FramePool().Take();
  FillFrameData(guest_frame, present_interval, *d, index);
  return d;
}

inline std::filesystem::path CapturesRoot() {
  const std::string given = REXCVAR_GET(wwe2k14_rdr_capture_dir);
  if (!given.empty()) return given;
  const auto exe = rex::filesystem::GetExecutableFolder();
  const auto up2 = exe.parent_path().parent_path();
  if (exe.parent_path().filename() == "build" && up2.filename() == "out") return up2 / "captures";
  return exe / "captures";
}

inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source);  // S101 lane17 (below)
inline void ResetGeoMemoForNewStore();  // S103 IXF (defined with g_geo_memo, below)
inline void StartCapture(uint32_t frames) {
  std::filesystem::path root = CapturesRoot();
  std::string label = REXCVAR_GET(wwe2k14_rdr_capture_label);
  if (label.empty()) label = "capture";
  std::filesystem::path folder = root / label;
  for (int n = 2; std::filesystem::exists(folder); ++n) folder = root / (label + "_" + std::to_string(n));
  const uint64_t budget = uint64_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_capture_budget_mb))) << 20;
  if (!g_cap.writer.Begin(folder, budget)) {
    REXLOG_ERROR("[rdr][capture] could not start one in {}", folder.string());
    return;
  }
  g_prov_on.store(true, std::memory_order_relaxed);  // S101 lane17: a capture (with or without live mode) gets provenance
  rdr_adapter::origin::Runs().Clear();  // S101 U1 review D2: a capture starts with room for its own run strings (ids already handed out stay valid)
  wwe2k14::prov::InstallWriterHook(&OnHostWriteBy);
  g_cap.active = true;
  g_cap.stop = false;
  g_cap.over_logged = false;
  g_cap.frames_left = frames;
  g_cap.frame_index = 0;
  g_cap.folder = folder;
  g_cap.geo.clear();
  ++g_geo_forget_epoch, ++g_geo_clear_epoch;
  ResetGeoMemoForNewStore();  // S103 IXF: the memo's "used this record frame" shortcut must not hand out the old store's offsets (frame 0 of a second capture)
  for (Image& img : g_reg.images) {
    img.content_written = false;
    img.checked_frame = ~0ull;
    img.written_in_capture = false;
  }
  REXLOG_INFO("[rdr][capture] started: {} frames into {} (budget {} MB)", frames, folder.string(), budget >> 20);
  if (REXCVAR_GET(wwe2k14_scene_records)) {  // S101 lane 26
    auto sink = std::make_unique<rdr_scene::live::FolderSink>();
    std::string error;
    if (sink->Begin(folder.string(), error)) {
      rdr_scene::live::AddSink(sink.get());
      g_cap.scene_sink = std::move(sink);
    } else {
      REXLOG_ERROR("[scene][live] could not start the capture's scene folder: {}", error);
    }
  }
}

// ---- the display table ----------------------------------------------------------------------------------------------------------------
// The library's display lookup emitter 0x82CBABB0 (r3 device, r4 three 256-entry u16 tables R / G / B at +0 / +512 / +1024 bytes, each value
// a 10-bit level in bits 6-15; listing: register 0x1925 gets R << 20 | G << 10 | B per entry). The table comes from the game's set-gamma
// call 0x82CAB6C8, which maps its ramp through the display's gamma (0x82CBAA90 -> VdGetCurrentDisplayGamma). The old path applies it at
// scan-out; without it our picture was lifted in the darks (S100 run 6852: game level 10 -> ours 23, 30 -> ~47, 100 -> ~115, 255 -> 255).
// A null r4 (the present's no-texture path 0x82CB357C) writes zeros for a frame with nothing to show: not taken.
// S103 P6: the table is parsed and kept in renderer_game\frame_tick.h (every build; the hook calls frame_tick::OnGammaRamp itself). The reader keeps
// only the null note; its live path takes a changed table from frame_tick::TheCurve() (OnPresent / TailLatch) as it took g_display_curve before.
inline void OnGammaRamp(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (ctx.r4.u32) return;
  std::lock_guard lock(g_mutex);
  Note("display_table_null");
}
// S103 P6: the frame boundary's present interval word is the same one frame_tick reads.
static_assert(kDevPresentInterval == wwe2k14::frame_tick::kDevPresentInterval);


