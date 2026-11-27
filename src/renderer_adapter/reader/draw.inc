
inline void DrawCommon(PPCContext& ctx, uint8_t* base, uint32_t dev, const DrawArgs& a) {
  Census& c = g_census;
  ++c.draws;
  if (a.inline_vertices) ++c.inlines;
  if (base[dev + 11068] & 0x31) Note("tiling_active");  // ADAPTER_CALLS rows 4, 20: not taken with force_no_msaa
  if (a.prim == Prim::Point) {  // P8
    Note("point_draw_dropped");
    return;
  }
  if (a.prim == Prim::Line || a.prim == Prim::LineStrip || a.prim == Prim::LineLoop) Note("line_draw");
  if (uint32_t(a.prim) == 0 || uint32_t(a.prim) > 15) {
    Note("prim_unknown", uint32_t(a.prim));
    return;
  }
  if (a.prim == Prim::TriFan || a.prim == Prim::Polygon || a.prim == Prim::QuadStrip || a.prim == Prim::TriW) Note("prim_outside_census", uint32_t(a.prim));
  if (!a.inline_vertices && !db::Ld32(base, dev + kDevIb)) {
    Note("indexed_draw_no_index_buffer");
    return;
  }
  const uint32_t vs_object = db::Ld32(base, dev + kDevVs), ps_object = db::Ld32(base, dev + kDevPs);
  if (!vs_object) {
    Note("draw_no_vertex_shader");
    return;
  }
  uint8_t strides[24];
  std::memcpy(strides, db::Host(base, dev + kDevStrides), 24);
  if (a.inline_vertices) strides[0] = uint8_t(a.inline_stride >> 2);
  const VertexShaderInfo* vs = nullptr;
  const PixelShaderInfo* ps = nullptr;
  {
    ScopedNs timer(g_times.shaders);
    if (g_fine_on) {
      FineTimes& f = g_fine;
      ++f.draws;
      const uint32_t decl = db::Ld32(base, dev + kDevDecl);
      if (vs_object == f.last_vs && decl == f.last_decl && std::memcmp(strides, f.last_strides, 24) == 0) ++f.same_vs_key;
      if (vs_object == f.last_vs && ps_object == f.last_ps) ++f.same_pair;
      f.last_vs = vs_object;
      f.last_ps = ps_object;
      f.last_decl = decl;
      std::memcpy(f.last_strides, strides, 24);
    }
    {
      FineNs fine(g_fine.vs_lookup);
      vs = GetVertexShader(ctx, base, dev, vs_object, strides);
    }
    FineNs fine(g_fine.ps_lookup);
    if (vs && ps_object) ps = GetPixelShader(base, ps_object);
  }
  if (!vs) return;
  if (ps_object) {
    // The variant-2 patch (ADAPTER_CALLS row 33): the interpolator masks of the two shaders differ. Our variant-0 facts are kept and the draw is counted.
    const uint32_t blk = ps_object + db::Ld32(base, ps_object + 64);
    const uint32_t rec0 = db::VariantRecord(base, vs_object, 0);
    constexpr uint32_t kInterpMask = 0x603FF;
    if ((db::Ld32(base, blk + 40 + 20) & kInterpMask) != (db::Ld32(base, rec0 + 872 + 20) & kInterpMask)) Note("vs_variant2_needed");
  }
  {
    ScopedNs timer(g_times.targets);
    EmitTargets(base, dev);
  }

  FrameTables& t = g_frame;
  cap::Draw d;
  d.vs_hash = vs->original_hash;
  d.vs_patched_hash = vs->patched_hash;
  d.ps_hash = ps ? ps->hash : 0;
  d.source_prim = uint8_t(a.prim);
  if (a.inline_vertices) d.flags |= cap::kDrawInline;

  // S102 T18 (wwe2k14_rdr_particle_lite): a hard-yParticle inline draw at a particle recorder site is a candidate; the gate (particle_lite.h) says whether it is the next draw of a run.
  namespace pl = rdr_adapter::particle_lite;
  std::optional<pl::Key> pl_key_opt;  // (nothing zero-filled on the off path)
  bool pl_candidate = false, pl_lite = false, pl_verify_taken = false, pl_thin_noquery = false;
  // S102 SW2 (wwe2k14_rdr_draw_lite bit 1): the same gate for every other draw (its own Gate and census: the key leaves the origin out, origin_same says whether it matches)
  const bool pl_particle_try = pl::g_on.load(std::memory_order_relaxed) && a.inline_vertices && ps && vs->original_hash == pl::kVs && ps->hash == pl::kPs;
  const bool pl_general = !pl_particle_try && pl::g_on_gen.load(std::memory_order_relaxed);
  pl::Gate* pl_gate = pl_general ? &pl::g_gate_gen : &pl::g_gate;
  bool pl_origin_copy = false;  // the origin record (and the thin query's skip) of the previous draw is this draw's too
  StateWords pl_words;          // the gate's own state gather (marks set), handed to the full path when the draw is declined: one gather per draw
  bool pl_have_words = false;
  if (pl_particle_try || pl_general) {
    rdr_adapter::origin::Key pk;
    const bool pk_ok = rdr_adapter::origin::Global().Resolve(rdr_adapter::origin::t_replay_tag, pk) && pk.op == rdr_adapter::origin::t_replay_op;
    if (pl_general || (pk_ok && (pk.call_site == pl::kSpriteSite || pk.call_site == pl::kQuadSite))) {
      pl_candidate = true;
      pl::Key& pl_key = pl_key_opt.emplace();
      if (pk_ok) pl_key.op = pk.op, pl_key.site = pk.call_site, pl_key.pass_site = pk.pass_site, pl_key.object = pk.object, pl_key.part = pk.part, pl_key.widget = pk.widget, pl_key.text = pk.text, pl_key.run = pk.run;
      else pl_key.origin_ok = false;
      pl_key.vs_object = vs_object, pl_key.ps_object = ps_object, pl_key.decl = db::Ld32(base, dev + kDevDecl), pl_key.prim = uint32_t(a.prim);
      pl_key.vs = vs, pl_key.ps = ps;
      std::memcpy(pl_key.strides, strides, sizeof(strides));
      // Modes that change a draw's records after the state is read: never lite (and the draw ends the run: Remember leaves the gate invalid).
      const bool excluded = !Recording() || g_wide.on() || g_jitter.on || TraceOn();
      if (!a.unmarked) PlLogMarks(pl_key);  // diagnosis: which setters raised the marks on this candidate (a cvar, off by default)
      const bool pl_taken = pl_gate->Decide(
          pl_key, a.unmarked, t.generation, t.draws.size(), t.ops.size(), excluded, pl::Entries(),
          [&] {  // the marks are set: the state words the full path gathers, against the state cache's (the by-marks path's own compare; ~280 bytes)
            const StateCache& sc = g_state_cache;
            if (!(sc.generation == t.generation && pl::StateEntryOk(sc.entry, pl::Entries()) && sc.words.prim == uint32_t(a.prim) && sc.words.has_ps == uint32_t(ps_object != 0) &&
                  sc.words.writes_depth == uint32_t(ps && ps->writes_depth)))
              return false;
            GatherStateWords(base, dev, a.prim, ps_object != 0, ps, pl_words);
            pl_have_words = true;  // (a declined draw's full path uses these words: S102 SW2)
            return std::memcmp(&pl_words, &sc.words, sizeof(pl_words)) == 0;
          },
          [&] { return PlSlotsSame(base, dev, pl_gate->snap); }) == pl::Reason::Taken;
      // F5: under --wwe2k14_rdr_debug_marks_verify the full path runs and what the lite path would have copied is compared with it below (particle_lite_miss).
      pl_lite = pl_taken && !REXCVAR_GET(wwe2k14_rdr_debug_marks_verify);
      pl_verify_taken = pl_taken && !pl_lite;
      pl_origin_copy = pl_taken && (!pl_general || (pl_gate->origin_same && pl_key.run == 0 && pl_key.widget == 0 && pl_key.text == 0));  // S102 SW2 (review F1): a glyph / widget / run draw's TakeDrawOrigin also pushes its TextRun and bumps the ui census: never copied
    }
  }
  struct PlPrev {
    uint32_t fixed = 0, dynamic = 0, texture_first = 0, texture_count = 0, layout = 0;
  } pl_prev;  // the run's previous draw: only the five fields the lite path reads (a copy: the tables grow below)
  if (pl_lite || pl_verify_taken) {
    const cap::Draw& pd = t.draws[pl_gate->draw_index];
    pl_prev.fixed = pd.fixed, pl_prev.dynamic = pd.dynamic, pl_prev.texture_first = pd.texture_first, pl_prev.texture_count = pd.texture_count, pl_prev.layout = pd.layout;
  }

  cap::DrawFixed fixed;
  std::memset(&fixed, 0, sizeof(fixed));
  cap::DrawDynamic dynamic;
  std::memset(&dynamic, 0, sizeof(dynamic));
  if (pl_lite) {  // S102 T18: what the by-marks path hands this draw: the state cache's records, which are the previous draw's
    d.fixed = pl_prev.fixed;
    d.dynamic = pl_prev.dynamic;
    ++g_times.state_reused;
  } else {
    ScopedNs state_timer(g_times.state);
    StateCache& sc = g_state_cache;
    // C7: no setter since the last draw (its marks), the same primitive and pixel shader facts: the last draw's state, the device unread.
    const bool by_marks = a.unmarked && pl::StateEntryOk(sc.entry, pl::Entries()) /* S102 T18 (C7 hole): no draw entered a hook and recorded nothing since the cache's draw */ && sc.generation == t.generation && sc.words.prim == uint32_t(a.prim) &&
                          sc.words.has_ps == uint32_t(ps_object != 0) && sc.words.writes_depth == uint32_t(ps && ps->writes_depth);
    const bool verify = by_marks && REXCVAR_GET(wwe2k14_rdr_debug_marks_verify);
    StateWords words_local;
    StateWords& words = pl_have_words ? pl_words : words_local;  // S102 SW2: the gate's gather of this draw, when it made one
    if (by_marks && !verify) {
      ++g_times.state_by_marks;
    } else if (!pl_have_words) {
      GatherStateWords(base, dev, a.prim, ps_object != 0, ps, words);
    }
    if (verify && std::memcmp(&words, &sc.words, sizeof(words)) != 0) {
      const uint64_t n = ++g_times.marks_missed;
      static uint32_t logged = 0;
      if (logged < 12) {
        ++logged;
        const uint8_t* x = reinterpret_cast<const uint8_t*>(&words);
        const uint8_t* y = reinterpret_cast<const uint8_t*>(&sc.words);
        size_t at = 0;
        while (at < sizeof(words) && x[at] == y[at]) ++at;
        REXLOG_WARN("[rdr][marks] miss #{}: unmarked draw whose state differs from the last draw's at byte {} of the state words (guest frame {})", n, at,
                    wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
      }
    }
    if (by_marks) {
      ++g_times.state_reused;
      if (verify) ++g_times.state_by_marks;
      d.fixed = sc.fixed;  // (verify: the pictures still follow the marks)
      d.dynamic = sc.dynamic;
    } else if (sc.generation == t.generation && std::memcmp(&words, &sc.words, sizeof(words)) == 0) {
      d.fixed = sc.fixed;  // the last draw's inputs, so its entries (S100 speed)
      d.dynamic = sc.dynamic;
      ++g_times.state_reused;
    } else {
      {
        FineNs fine(g_fine.fixed_decode);
        DecodeFixed(base, dev, a.prim, ps_object != 0, db::Ld32(base, dev + kDevDs), fixed);
      }
      {
        FineNs fine(g_fine.fixed_dedupe);
        d.fixed = Dedupe(t.fixeds, t.fixed_map, fixed, reinterpret_cast<const uint8_t*>(&fixed), sizeof(fixed));
      }
      {
        FineNs fine(g_fine.dyn_decode);
        dynamic.ndc_scale[0] = dynamic.ndc_scale[1] = dynamic.ndc_scale[2] = 1;
        DecodeDynamic(base, dev, ps, dynamic);
      }
      {
        FineNs fine(g_fine.dyn_dedupe);
        d.dynamic = Dedupe(t.dynamics, t.dynamic_map, dynamic, reinterpret_cast<const uint8_t*>(&dynamic), sizeof(dynamic));
      }
      sc.generation = t.generation;
      sc.words = words;
      sc.fixed = d.fixed;
      sc.dynamic = d.dynamic;
    }
    if (g_wide.on()) ApplyWide(base, dev, a, vs, vs->patched_hash, d);  // settings parity S5 (the SDK's hashes = our patched ones)
    if (g_jitter.on) ApplyJitter(base, dev, a.prim, d, vs->patched_hash, LiveFullViewRect(base, dev, a));  // settings parity S10b
    if (g_fine_on) {
      g_fine.same_fixed += d.fixed == g_fine.last_fixed;
      g_fine.same_dynamic += d.dynamic == g_fine.last_dynamic;
      g_fine.last_fixed = d.fixed;
      g_fine.last_dynamic = d.dynamic;
    }
  }
  g_state_cache.entry = pl::Entries();  // S102 T18 (C7 hole): the state this draw recorded is what the cache holds (a lite draw's state is the cache's: the same facts)

  // Texture slots: only those a shader of this draw fetches.
  d.texture_first = uint32_t(t.texture_slots.size());
  if (pl_lite) {  // S102 T18: the previous draw's slot records again (one record per slot per draw, as the full path appends)
    cap::TextureSlot lite_slots[32];
    const uint32_t n = pl_prev.texture_count < 32 ? pl_prev.texture_count : 32;
    for (uint32_t k = 0; k < n; ++k) lite_slots[k] = t.texture_slots[pl_prev.texture_first + k];
    for (uint32_t k = 0; k < n; ++k) t.texture_slots.push_back(lite_slots[k]);
    g_times.slots_reused += n;
    // S102 SW2 (review F2): the full path's slot hit calls EnsureContent(image, words) for every textured slot (a copy destination every draw: a CPU rewrite between two draws is a new content ref; the others return at once).
    // The snapshot's words are the ones the hit path reads (PlSnapOne = AddTextureSlot's source), proven equal to the live state by Decide.
    if (pl_general)
      for (uint32_t k = 0; k < pl_gate->snap.n; ++k)
        if (pl_gate->snap.s[k].image) EnsureContentChecked(pl_gate->snap.s[k].image, pl_gate->snap.s[k].w);
  } else {
    ScopedNs slots_timer(g_times.slots);
    for (uint32_t slot = 0; slot < 32; ++slot) {
      const TFetch& a_t = vs->facts.tfetch[slot];
      const TFetch* tf = a_t.used ? &a_t : nullptr;
      if (ps && ps->facts.tfetch[slot].used) {
        if (tf) Note("texture_slot_read_by_vs_and_ps");
        tf = &ps->facts.tfetch[slot];
      }
      if (tf) {
        FineNs fine(g_fine.slot_add);
        g_fine.slots += g_fine_on;
        ++g_times.slot_adds;
        AddTextureSlot(base, dev, slot, *tf);
      }
    }
  }
  d.texture_count = uint32_t(t.texture_slots.size()) - d.texture_first;
  if (TraceOn()) {
    std::string slots;
    for (uint32_t k = d.texture_first; k < t.texture_slots.size(); ++k) {
      const cap::TextureSlot& s = t.texture_slots[k];
      uint32_t w[6];
      for (uint32_t j = 0; j < 6; ++j) w[j] = db::Ld32(base, dev + kDevFetch + 24 * s.slot + 4 * j);
      const GpuFetchSlot& g = g_gpu_fetch[s.slot < 32 ? s.slot : 0];
      slots += " s" + std::to_string(s.slot) + (s.image ? "=img" + std::to_string(s.image) : std::string("=EMPTY")) + " dev[" + TraceWords(w) + "] gpu[" +
               (g.known ? TraceWords(g.words) : std::string("?")) + "]";
    }
    REXLOG_INFO("[rdr][trace] draw #{} {} vs {:016X} ps {:016X} prim {} count {}{}", g_trace_draw++, a.inline_vertices ? "inline" : "indexed", vs->original_hash,
                ps ? ps->hash : 0ull, uint32_t(a.prim), a.inline_vertices ? a.inline_count : a.index_count, slots);
  }

  // Layout.
  if (pl_lite) {  // S102 T18: the same vertex shader as the previous draw
    d.layout = pl_prev.layout;
  } else if (FineNs layout_fine(g_fine.layout); !vs->elements.empty()) {
    cap::VertexLayout layout;
    layout.element_first = uint32_t(t.elements.size());
    layout.element_count = uint32_t(vs->elements.size());
    layout.host_stride = vs->host_stride;
    layout.guest_stride = vs->guest_stride;
    const uint64_t key = vs->layout_hash;
    if (auto it = t.layout_map.find(key); it != t.layout_map.end()) {
      d.layout = it->second;
    } else {
      d.layout = uint32_t(t.layouts.size());
      t.layouts.push_back(layout);
      t.elements.insert(t.elements.end(), vs->elements.begin(), vs->elements.end());
      t.layout_map[key] = d.layout;
    }
  }

  GeoJob* pending_geometry = nullptr;
  cap::ThinDrawFacts thin_facts_now;  // lane 122 (review 6): the identity facts of this draw, carried beside it in kSecThinFacts
  if (Recording()) {
    if (const int32_t spin = REXCVAR_GET(wwe2k14_rdr_debug_spin_us); spin > 0) {  // diagnosis: a pause per draw under the lock, nothing read
      // render-clock-begin: dev - test-only spin that slows the reader on purpose
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin);
      while (std::chrono::steady_clock::now() < until) {
      }
      // render-clock-end
    }
    if (!(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 2)) {
      ScopedNs timer(g_times.banks);
      d.vs_bank = PushBanks(base, dev, vs, ps_object, d.ps_bank);
    }
    {
      FineNs fine(g_fine.bool_loop);
      StateCache& sc = g_state_cache;
      const uint8_t* raw = db::Host(base, dev + kDevBoolLoop);
      if (sc.bool_generation == t.generation && std::memcmp(raw, sc.bool_raw, sizeof(sc.bool_raw)) == 0) {
        d.bool_loop = sc.bool_loop;  // S100 speed: the last draw's words, so its entry
        ++g_times.bool_reused;
      } else {
        cap::BoolLoop bl;
        for (uint32_t i = 0; i < 40; ++i) bl.words[i] = db::Ld32(base, dev + kDevBoolLoop + 4 * i);
        d.bool_loop = Dedupe(t.bool_loops, t.bool_map, bl, reinterpret_cast<const uint8_t*>(&bl), sizeof(bl));
        std::memcpy(sc.bool_raw, raw, sizeof(sc.bool_raw));
        sc.bool_generation = t.generation;
        sc.bool_loop = d.bool_loop;
      }
    }
    GeoRef g;
    GeoResult built = GeoResult::kNone;
    if (rdr_adapter::thin::FactsTableOn()) {  // lane 129 (review F1: the one predicate); review 6: free identity facts of the draw (its own arguments and the slots already walked)
      if (!a.inline_vertices) {   // review F4: the vertex fetch constant (address, size): the draw's own vertex stream
        const uint32_t vat = dev + kDevFetch + 24 * (vs->fetch_constant / 3) + 8 * (vs->fetch_constant % 3);
        thin_facts_now.vb_addr = db::Ld32(base, vat), thin_facts_now.vb_dw1 = db::Ld32(base, vat + 4);
      }
      thin_facts_now.index_addr = a.inline_vertices ? 0 : db::Ld32(base, dev + kDevIb);
      thin_facts_now.index_count = a.index_count, thin_facts_now.base_vertex = uint32_t(a.base_vertex), thin_facts_now.start_index = a.start_index;
      for (uint32_t k = d.texture_first; k < t.texture_slots.size(); ++k) {
        const cap::TextureSlot& s = t.texture_slots[k];
        if (s.slot == 0) thin_facts_now.img0 = s.image;
        else if (s.slot == 4) thin_facts_now.img4 = s.image;
        else if (s.slot == 5) thin_facts_now.img5 = s.image;
      }
    }
    // S102 T18: the previous draw of the run returned before the thin query (its answer is a function of the origin facts in the key): the same answer, nothing to count
    const bool pl_skip_thin = pl_lite && pl_origin_copy && pl_gate->SkipThin();
    if (pl_skip_thin) pl_thin_noquery = true;
    const bool thin_lite = rdr_adapter::thin::FactsTableOn() && !pl_skip_thin && ThinLiteDraw(t, a.inline_vertices != nullptr, vs->original_hash, ps ? ps->hash : 0, thin_facts_now, &pl_thin_noquery);  // lane 122: a pinned key's draw: no geometry build (the binder draws its template)
    if (!thin_lite && !(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 1)) {
      ScopedNs timer(g_times.geometry);
      built = BuildGeometry(base, dev, a, *vs, g, pending_geometry);
    }
    if (built == GeoResult::kBuilt) ApplyGeometry(d, g);
    else if (built == GeoResult::kNone && !thin_lite) d.flags |= cap::kDrawNoGeometry;
    if (thin_lite) d.flags |= cap::kDrawThin;  // lane 122: the anchor only (hashes and counts of a lite draw are not read by the binder)
    // kPending: on a worker; JoinGeometryJobs patches this draw before the frame is serialized
    if (g_cap.active) CheckWriter();
  } else {
    d.flags |= cap::kDrawNoGeometry;
  }
  if (pending_geometry) pending_geometry->draws.push_back(uint32_t(t.draws.size()));
  t.ops.push_back({cap::OpKind::Draw, uint32_t(t.draws.size())});
  t.draws.push_back(d);
  if (rdr_adapter::thin::FactsTableOn()) t.thin_facts.push_back(thin_facts_now);   // review F1  // lane 122: parallel to draws
  size_t pl_runs_before = 0;
  if (pl_lite && pl_origin_copy) {  // S102 T18: the origin of the same key is the previous draw's record; the names census counts it as TakeDrawOriginInto would
    ++pl_gate->census->origin_copied;
    const cap::DrawOrigin lite_origin = t.origins[pl_gate->draw_index];
    ++rdr_adapter::names_draw::g_names.draws;
    ++rdr_adapter::names_draw::g_names.by_class[uint8_t(lite_origin.klass)];
    t.origins.push_back(lite_origin);
  } else {
    pl_runs_before = t.text_runs.size();  // S102 SW2 (F1, live verify): did the full path push a TextRun for this draw
    t.origins.push_back(TakeDrawOrigin(t));  // S101 names
  }
  if (pl_candidate) {  // S102 T18
    if (pl_verify_taken) {  // F5: what the lite path would have copied, against what the full path just recorded
      const bool same = d.fixed == pl_prev.fixed && d.dynamic == pl_prev.dynamic && d.layout == pl_prev.layout && d.texture_count == pl_prev.texture_count &&
                        (d.texture_count == 0 || std::memcmp(&t.texture_slots[d.texture_first], &t.texture_slots[pl_prev.texture_first], d.texture_count * sizeof(cap::TextureSlot)) == 0) &&
                        (!pl_origin_copy || (std::memcmp(&t.origins.back(), &t.origins[pl_gate->draw_index], sizeof(cap::DrawOrigin)) == 0 && t.text_runs.size() == pl_runs_before));  // (F1: a copied origin must not have skipped a TextRun)
      if (!same) {
        const uint64_t n = ++pl_gate->census->misses;
        static uint32_t pl_logged = 0;
        if (pl_logged < 12) {
          ++pl_logged;
          REXLOG_WARN("[rdr][{}] miss #{}: a taken draw's full record differs from the previous draw's (fixed {}/{}, dynamic {}/{}, layout {}/{}, slots {}/{}; guest frame {})", pl_general ? "draw_lite" : "particle_lite", n, d.fixed, pl_prev.fixed, d.dynamic,
                      pl_prev.dynamic, d.layout, pl_prev.layout, d.texture_count, pl_prev.texture_count, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
        }
      }
    }
    if (pl_lite) {  // S102 SW2: a taken draw's slots are the snapshot's own (Decide just proved them): nothing to rebuild
      pl_gate->RememberTaken(*pl_key_opt, t.draws.size() - 1, t.ops.size(), pl_thin_noquery, pl::Entries());
    } else {
      pl::SlotsSnap pl_snap;
      PlSnapSlots(base, dev, t.texture_slots.data() + d.texture_first, d.texture_count, pl_snap);
      pl_gate->Remember(*pl_key_opt, t.generation, t.draws.size() - 1, t.ops.size(), pl_thin_noquery, pl::Entries(), pl_snap);
    }
  }
}

// lane 152 (D3-F, SKIP_GAME_WORK_MAP.md 10.7 item 6): the D3 MARKER. The hook's op-15 record (r4 = 6, r5 = the hook's guest frame, r6 = the mark generation, r7 = 0) replays as a draw whose origin call_site is
// kMarkerCall | ordinal. It is a 2-index strip as recorded, so DrawCommon would reject it ("no index buffer" / "no vertex shader") or build geometry for it: it never goes there. The marker becomes a Draw with
// kDrawNoGeometry, no shaders, the PREVIOUS draw's state (the device's shadow at that point), and the frame / generation in vertex_offset / index_offset (what the binder's BuildMini reads). Returns true when the
// draw was a marker (handled). UNPROVEN LIVE: never run (0 game runs); the unit path is tools\d3\d3_check.py's emulation, which builds the same Draw.
inline bool TryD3Marker(const DrawArgs& a) {
  if (!rdr_adapter::d3::AnyD3Chain() || !Recording()) return false;
  namespace og = rdr_adapter::origin;
  og::Key k;
  if (!og::Global().Resolve(og::t_replay_tag, k) || k.op != og::t_replay_op || !rdr_adapter::d3::MarkerClaimed(k.call_site)) return false;   // lane 217 (E2b): a caster marker is claimed like the arena's; lane 246 (16.8 precondition 4): a ring marker too (d3_arena.h MarkerClaimed, unit-tested), or it would draw as a geometry-store offset (a hole)
  if (!rdr_adapter::d3::IsMarkerReplay(uint32_t(a.prim), a.index_count, a.inline_vertices)) return false;   // lane 204 (Check K1): the marker's arguments AS REPLAYED: r4 = 6, and the recorded r7 = 0 arrives as 2 (op 15 adds the table's 2: d3_arena.h kMarkerReplayIndexCount); a real draw with a stale tag is passed on, never claimed
  const uint64_t g = wwe2k14::g_guest_frame.load(std::memory_order_relaxed), mf = uint32_t(a.base_vertex);
  if (mf + 6 < uint32_t(g) || mf > uint32_t(g)) {   // (N4) a marker names the hook's frame, which is at most a few frames behind the counter: anything else is no marker of ours; consumed and counted, never built as a draw
    Note("d3_marker_bad_frame");
    return true;
  }
  skips_auto::NoteMarker();   // S102 K3 review F2: this frame carries a marker: it is bound with the chains whatever the timing window says (SubmitFrame: ChainFor)
  FrameTables& t = g_frame;
  cap::Draw m;
  if (!t.draws.empty()) m = t.draws.back();
  m.flags = cap::kDrawNoGeometry;
  m.vs_hash = m.ps_hash = m.vs_patched_hash = 0;
  m.texture_first = uint32_t(t.texture_slots.size()), m.texture_count = 0;
  m.vertex_bytes = m.index_bytes = m.vertex_count = m.index_count = 0;
  m.layout = 0xFFFFFFFFu;
  m.vertex_offset = uint64_t(uint32_t(a.base_vertex));   // the hook's guest frame (low 32 bits: the packet's key)
  m.index_offset = uint64_t(a.start_index);              // the mark generation the hook read
  // lane 289 (the ring SKIP arm): the skipped call's draws never arrive, so its part triples (the sequence the binder published with the mark set the hook read, found by the generation this marker carries) join the thin ordinal
  // counters HERE, at the marker's position: the instances after a cut keep their ordinals (the binder's ComputeKeys fills the same list at the same draw). The VERIFY arm drops nothing: nothing to fill.
  if (rdr_adapter::d3::IsRingMarkerCall(k.call_site) && rdr_adapter::d3::g_ring_d3 && !rdr_adapter::d3::g_ring_verify && rdr_adapter::thin::FactsTableOn()) {
    ThinLiteBegin(t);
    if (t.lite.active()) {
      const std::shared_ptr<const rdr_adapter::d3::MarkSet> ms = rdr_adapter::d3::g_rmarks.Find(a.start_index);
      const std::vector<uint64_t>* parts = ms ? ms->PartsOf(rdr_adapter::d3::MarkKey(k.object, k.call_site & 0xFFu)) : nullptr;
      if (parts) {
        t.lite.Reserve(parts->data(), parts->size());
        rdr_adapter::d3::g_ring_fills.fetch_add(1, std::memory_order_relaxed);
      } else {
        rdr_adapter::d3::g_ring_fill_missing.fetch_add(1, std::memory_order_relaxed);
        Note("d3_ring_fill_missing");
      }
    }
  }
  t.ops.push_back({cap::OpKind::Draw, uint32_t(t.draws.size())});
  t.draws.push_back(m);
  if (rdr_adapter::thin::FactsTableOn()) t.thin_facts.push_back(cap::ThinDrawFacts{});
  t.origins.push_back(TakeDrawOrigin(t));
  Note("d3_marker_claimed");   // lane 204
  return true;
}

// 0x82CB19E0 (r3 device, r4 primitive, r5 base vertex, r6 start index, r7 index count; d3d_layer.h).
inline bool OnDrawIndexed(PPCContext& ctx, uint8_t* base) {   // lane 152 (S2): true = a D3 marker was consumed (the caller must not run the writer for it)
  if (const int32_t spin = REXCVAR_GET(wwe2k14_rdr_debug_spin_us); spin < 0) {  // diagnosis: the same pause per draw, OUTSIDE the lock
    // render-clock-begin: dev - test-only spin that slows the reader on purpose
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(-spin);
    while (std::chrono::steady_clock::now() < until) {
    }
    // render-clock-end
  }
  std::lock_guard lock(g_mutex);
  ScopedNs timer(g_times.draw);
  wwe2k14::cost::ReaderSpan cost_span;  // S101 lane 83: from the lock; the class is set after the draw (one relaxed load when off)
  SyncGpuFetch(base, ctx.r3.u32);  // before the writer flushes and clears the mask
  DrawArgs a;
  a.prim = Prim(ctx.r4.u32 & 0x3F);
  a.base_vertex = int32_t(ctx.r5.u32);
  a.start_index = ctx.r6.u32;
  a.index_count = ctx.r7.u32;
  if (MarksOn()) a.unmarked = ReadMarks(base, ctx.r3.u32);
  const bool d3_marker = TryD3Marker(a);
  if (!d3_marker) {
    if (rdr_adapter::d3::AnyD3Chain()) {   // lane 204: a draw that still carries a marker tag but was not claimed (the census the K1 re-run reads: must be 0)
      rdr_adapter::origin::Key mk;
      if (rdr_adapter::origin::Global().Resolve(rdr_adapter::origin::t_replay_tag, mk) && mk.op == rdr_adapter::origin::t_replay_op &&
          rdr_adapter::d3::IsAnyMarkerCall(mk.call_site)) Note("d3_marker_unclaimed");   // lane 217: + a caster marker; lane 246: + a ring marker (d3_arena.h IsAnyMarkerCall)
    }
    DrawCommon(ctx, base, ctx.r3.u32, a);   // lane 152: a D3 marker is not a draw to build
  }
  if (wwe2k14::cost::On()) {  // the replayed record's tag is still set (the replay handler's run): resolve its class
    rdr_adapter::origin::Key k;
    wwe2k14::cost::Cls c = wwe2k14::cost::kOther;
    if (rdr_adapter::origin::Global().Resolve(rdr_adapter::origin::t_replay_tag, k)) {
      const rdr_adapter::origin::FnRow* sub = rdr_adapter::origin::FindRow(rdr_adapter::origin::kSubmitters, k.call_site);
      c = wwe2k14::cost::ClassOfDraw(k.object, k.pass_site, sub && sub->klass == cap::DrawClass::Crowd);
    }
    cost_span.SetClass(c);
  }
  return d3_marker;
}

// The vertex bytes exist only after Begin (the game fills them): Begin only remembers its arguments, the draw is read at End.
struct OpenInline {
  bool active = false;
  uint32_t device = 0, prim = 0, count = 0, stride = 0;
  bool unmarked = false;  // C7: the marks at Begin
};
inline thread_local OpenInline t_inline;

inline void OnBeginVertices(PPCContext& ctx, uint8_t* base) {
  {
    std::lock_guard lock(g_mutex);
    if (t_inline.active) Note("inline_begin_while_open");
    SyncGpuFetch(base, ctx.r3.u32);  // Begin is where the inline draw flushes its state (0x82CB1570), End only reads the vertices
    NoteDeviceFloatMasks(base, ctx.r3.u32);  // S100 speed: the constant marks before Begin's flush (or the native layer) clears them
  }
  bool unmarked = false;
  if (MarksOn()) {
    const uint32_t dev = ctx.r3.u32;
    const bool stride_bind = ((ctx.r6.u32 >> 2) & 0xFF) != base[dev + 12264] && db::Ld32(base, dev + 13072) != 0;
    unmarked = ReadMarks(base, dev, stride_bind);
  }
  t_inline = {true, ctx.r3.u32, ctx.r4.u32 & 0x3F, ctx.r5.u32, ctx.r6.u32, unmarked};
}

inline void OnEndVertices(PPCContext& ctx, uint8_t* base) {
  OpenInline o = t_inline;
  t_inline.active = false;
  if (!o.active) return;
  std::lock_guard lock(g_mutex);
  ScopedNs timer(g_times.draw);
  const uint32_t dev = o.device;
  const uint32_t data = db::Ld32(base, dev + kDevInlinePtr);
  const uint32_t dwords = db::Ld32(base, dev + kDevInlineDwords);
  if (!data || !o.count || !o.stride) {
    Note("inline_draw_empty");
    return;
  }
  if (dwords * 4 < o.count * o.stride) Note("inline_space_smaller_than_vertices");
  DrawArgs a;
  a.prim = Prim(o.prim);
  a.inline_vertices = db::Host(base, data);
  a.inline_count = o.count;
  a.inline_stride = o.stride;
  a.unmarked = o.unmarked && NoMarks(base, dev);  // (C7: and no setter between Begin and End)
  DrawCommon(ctx, base, dev, a);
}

// 0x82CB52B8 Clear (d3d_layer_copy.h; C26): r3 device, r4 rect count, r5 rects or 0, r6 flags, r7 colour, f1 depth, r9 stencil.
inline void OnClear(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  ++g_census.clears;
  const uint32_t dev = ctx.r3.u32, count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32, argb = ctx.r7.u32;
  if (flags & ~0x3Fu) Note("clear_flags_unknown");
  if (base[dev + 11068] & 0x31) Note("tiling_active");
  const uint32_t ds = db::Ld32(base, dev + kDevDs);
  uint32_t eff = flags & 0x3F;
  if (!ds) eff &= ~0x30u;
  const uint32_t rt0 = db::Ld32(base, dev + kDevRt0);
  const uint32_t size_surface = rt0 ? rt0 : ds;
  if (!size_surface) {
    Note("clear_no_surface");
    return;
  }
  EmitTargets(base, dev);
  const uint32_t size_word = db::Ld32(base, size_surface + 36);
  const int32_t width = int32_t((size_word >> 18) & 0x3FFF) + 1, height = int32_t((size_word >> 3) & 0x7FFF) + 1;
  const int32_t vx = int32_t(F(db::Ld32(base, dev + kDevViewport))), vy = int32_t(F(db::Ld32(base, dev + kDevViewport + 4)));
  const int32_t vw = int32_t(F(db::Ld32(base, dev + kDevViewport + 8))), vh = int32_t(F(db::Ld32(base, dev + kDevViewport + 12)));
  int32_t cx0 = std::max(0, vx), cy0 = std::max(0, vy), cx1 = std::min(width, vx + vw), cy1 = std::min(height, vy + vh);
  if (db::Ld32(base, dev + kDevScissorOn)) {
    cx0 = std::max(cx0, int32_t(db::Ld32(base, dev + kDevScissor)));
    cy0 = std::max(cy0, int32_t(db::Ld32(base, dev + kDevScissor + 4)));
    cx1 = std::min(cx1, int32_t(db::Ld32(base, dev + kDevScissor + 8)));
    cy1 = std::min(cy1, int32_t(db::Ld32(base, dev + kDevScissor + 12)));
  }

  rdr::ClearOp op;
  // C26: D3DCOLOR to floats; depth and stencil go with the first colour pass only.
  op.color[0] = float((argb >> 16) & 0xFF) / 255.0f;
  op.color[1] = float((argb >> 8) & 0xFF) / 255.0f;
  op.color[2] = float(argb & 0xFF) / 255.0f;
  op.color[3] = float((argb >> 24) & 0xFF) / 255.0f;
  op.depth = float(ctx.f1.f64);
  op.stencil = uint8_t(ctx.r9.u32);

  uint32_t passes[4];
  uint32_t pass_count = 0;
  if (eff & 0xF) {
    for (uint32_t i = 0; i < 4; ++i) {
      if ((eff & (1u << i)) && db::Ld32(base, dev + kDevRt0 + 4 * i)) passes[pass_count++] = i;
    }
    if (!pass_count) {
      Note("clear_colour_flags_no_target");  // the original does nothing, not even depth (C26)
      return;
    }
  } else if (eff & 0x30) {
    passes[pass_count++] = 0xFFFFFFFFu;
  } else {
    return;
  }
  if (rects && count == 0) return;  // the library's loop runs zero times
  if (count > 0 && rects) Note("clear_with_rectangles");
  const uint32_t rect_count = (count > 0 && rects) ? count : 1;
  for (uint32_t r = 0; r < rect_count; ++r) {
    int32_t x0 = cx0, y0 = cy0, x1 = cx1, y1 = cy1;
    if (count > 0 && rects) {  // (layout: D3DRECT x1 y1 x2 y2, unconfirmed: counted above)
      x0 = std::max(x0, int32_t(db::Ld32(base, rects + 16 * r)));
      y0 = std::max(y0, int32_t(db::Ld32(base, rects + 16 * r + 4)));
      x1 = std::min(x1, int32_t(db::Ld32(base, rects + 16 * r + 8)));
      y1 = std::min(y1, int32_t(db::Ld32(base, rects + 16 * r + 12)));
    }
    if (x1 <= x0 || y1 <= y0) {
      Note("clear_empty");
      continue;
    }
    op.full = x0 == 0 && y0 == 0 && x1 == width && y1 == height;
    op.rect = {x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0)};
    for (uint32_t p = 0; p < pass_count; ++p) {
      op.flags = 0;
      op.color_mask = 0;
      if (passes[p] != 0xFFFFFFFFu) {
        op.flags |= rdr::kClearColor;
        op.color_mask = uint8_t(1u << passes[p]);
      }
      if (p == 0) {
        if (eff & 0x10) op.flags |= rdr::kClearDepth;
        if (eff & 0x20) op.flags |= rdr::kClearStencil;
      }
      g_frame.ops.push_back({cap::OpKind::Clear, uint32_t(g_frame.clears.size())});
      g_frame.clears.push_back(op);
    }
  }
}
