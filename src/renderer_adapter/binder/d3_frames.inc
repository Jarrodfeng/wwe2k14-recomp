  // ---- lane 138: D3 skip the object (d3_arena.h, d3_binder.h, docs\renderer\SKIP_GAME_WORK_MAP.md 8.3) --------------------------------------------------------------------------------------
  void D3BeginFrame() {
    d3_pass_ready_ = false;
    d3_.BeginBind();   // lane 204 (P)
    d3_base_ = d3_.c;
    ++d3_.c.frames;
  }
  d3::Hooks D3Hooks() {
    d3::Hooks h;
    h.variant = [](uint64_t hash) { return FindVariant(hash); };
    h.pinned = [this](uint64_t key) { return thin_store_.Pinned(key); };
    return h;
  }
  // A marker: the object's parts are drawn from its sequence, the packet and the pass bank, through BindDraw as thin draws of a mini frame (draws the stream never carried).
  template <class Source>
  [[gnu::noinline]] void D3Marker(const LoadedFrame& frame, uint32_t draw_index, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    if (!d3_pass_ready_) {
      d3::D3Core::PassBank(frame, d3_pass_vs_, d3_pass_ps_, d3_has_vs_, d3_has_ps_);
      d3_pass_ready_ = true;
    }
    const d3::D3Core::MarkerResult r = d3_.BuildMini(frame, draw_index, &d3_pass_vs_, &d3_pass_ps_, d3_has_vs_, d3_has_ps_, d3_mini_);
    if (r != d3::D3Core::MarkerResult::Ok) {
      ++stats.settings_out;   // (a hole / no sequence / an epoch drop: counted by BuildMini; the marker draws nothing)
      return;
    }
    std::swap(thin_keys_, d3_mini_.keys), std::swap(thin_full_, d3_mini_.fulls);
    d3_overlay_ = &d3_mini_.overlay;
    for (uint32_t k = 0; k < d3_mini_.frame.draws.count; ++k) BindDraw(d3_mini_.frame, d3_mini_.frame.draws[k], capture, builder, bytes, stats);
    d3_overlay_ = nullptr;
    std::swap(thin_keys_, d3_mini_.keys), std::swap(thin_full_, d3_mini_.fulls);
  }
  // ---- lane 217 (E2b): the caster class's chain (the same core with route Casters; the arena's functions above are not shared with it) ------------------------------------------------------------------
  void D3CBeginFrame() {
    cc_->d3c_pass_ready_ = false;
    cc_->d3c_.BeginBind();
    cc_->d3c_base_ = cc_->d3c_.c;
    ++cc_->d3c_.c.frames;
  }
  // lane 221: the highest bone id of the draw's vertices + 1 (see d3::Hooks::slots_used). Cached per (vertex buffer range, layout): a part's vertices do not change between its draws of a frame.
  template <class Source>
  uint32_t D3CSlotsUsed(const LoadedFrame& frame, const Draw& dr, const Source& capture) { return D3SlotsUsedWith(cc_->d3c_slots_cache_, frame, dr, capture); }
  template <class Source>
  uint32_t D3SlotsUsedWith(std::unordered_map<uint64_t, uint32_t>& cache, const LoadedFrame& frame, const Draw& dr, const Source& capture) {   // lane 227: the casters' rule, one cache per chain
    if (dr.layout >= frame.layouts.count || dr.vertex_bytes == 0) return 0;
    const VertexLayout& vl = frame.layouts[dr.layout];
    if (uint64_t(vl.element_first) + vl.element_count > frame.elements.count || vl.host_stride < 4) return 0;
    int off = -1;
    for (uint32_t k = 0; k < vl.element_count; ++k) {
      const VertexElement& e = frame.elements[vl.element_first + k];
      if (e.source_format == 6 && (e.flags & 2) && e.vk_format == 41) off = e.host_offset;   // the integer 8888 element of VK_FORMAT_R8G8B8A8_UINT = the bone ids (shadow_caster_vertex.h Role::Ids)
    }
    if (off < 0 || uint32_t(off) + 4 > vl.host_stride) {
      if (d3::DebugOn()) {
        std::fprintf(stderr, "[d3dbg] slots_used: no bone-id element in layout (stride %u):", vl.host_stride);
        for (uint32_t k = 0; k < vl.element_count; ++k) {
          const VertexElement& e = frame.elements[vl.element_first + k];
          std::fprintf(stderr, " [src %u flags %u vk %u host %u]", unsigned(e.source_format), unsigned(e.flags), unsigned(e.vk_format), unsigned(e.host_offset));
        }
        std::fprintf(stderr, "\n");
      }
      return 0;
    }
    const uint64_t key = thin::Mix(thin::Mix(thin::Mix(0x51075, dr.vertex_offset), dr.vertex_bytes), (uint64_t(vl.host_stride) << 16) | uint32_t(off));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::vector<uint8_t> vb;
    uint32_t used = 0;
    if (capture.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vb) && vb.size() >= dr.vertex_bytes) {
      uint32_t mx = 0;
      for (size_t at = size_t(off); at + 4 <= vb.size(); at += vl.host_stride)
        for (int k = 0; k < 4; ++k)
          if (vb[at + k] != 255) mx = std::max<uint32_t>(mx, vb[at + k]);   // 255 = no influence (measured on run 7147: bytes 1-3 of a one-influence vertex)
      used = mx + 1;
    }
    if (used == 0) return 0;   // a failed read is not cached (the geometry may arrive later)
    if (cache.size() > 4096) cache.clear();
    cache[key] = used;
    return used;
  }
  template <class Source>
  d3::Hooks D3CHooks(const LoadedFrame& frame, const Source& capture) {
    d3::Hooks h;
    h.variant = [](uint64_t hash) { return FindVariant(hash); };
    h.palette_count = cc_->d3c_palette_count_;
    h.geometry = cc_->d3c_geometry_;
    h.slots_used = [this, &capture](const LoadedFrame& f, const Draw& dr) { return D3CSlotsUsed(f, dr, capture); };
    (void)frame;
    return h;
  }
  // A caster marker: the pass entry's parts are drawn from its sequence and the packet, as thin-less ordinary draws of a mini frame (their keys are zero: no thin store entry exists for a caster part).
  template <class Source>
  [[gnu::noinline]] void D3CMarker(const LoadedFrame& frame, uint32_t draw_index, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    if (!cc_->d3c_pass_ready_) {   // a caster pass has no pass bank (every camera register is a record register; Observe does the same)
      cc_->d3c_pass_vs_.fill(0), cc_->d3c_pass_ps_.fill(0), cc_->d3c_has_vs_.assign(256, 0), cc_->d3c_has_ps_.assign(256, 0);
      cc_->d3c_pass_ready_ = true;
    }
    const d3::D3Core::MarkerResult r = cc_->d3c_.BuildMini(frame, draw_index, &cc_->d3c_pass_vs_, &cc_->d3c_pass_ps_, cc_->d3c_has_vs_, cc_->d3c_has_ps_, cc_->d3c_mini_);
    if (r != d3::D3Core::MarkerResult::Ok) {
      ++stats.settings_out;
      return;
    }
    cc_->d3c_mini_.keys.assign(cc_->d3c_mini_.frame.draws.count, 0);   // no thin key: ThinFind / ThinAfter see an ordinary draw
    cc_->d3c_mini_.fulls.assign(cc_->d3c_mini_.frame.draws.count, thin::ThinKey{});
    std::swap(thin_keys_, cc_->d3c_mini_.keys), std::swap(thin_full_, cc_->d3c_mini_.fulls);
    d3c_real_frame_ = &frame;   // lane 228: the hooks' FrameReady / object joins take the real frame, not the mini frame
    for (uint32_t k = 0; k < cc_->d3c_mini_.frame.draws.count; ++k) BindDraw(cc_->d3c_mini_.frame, cc_->d3c_mini_.frame.draws[k], capture, builder, bytes, stats);
    d3c_real_frame_ = nullptr;
    std::swap(thin_keys_, cc_->d3c_mini_.keys), std::swap(thin_full_, cc_->d3c_mini_.fulls);
  }
  template <class Source>
  void D3CEndFrame(const LoadedFrame& frame, const Source& capture) {
    std::vector<d3::Run> runs;
    { d3t::Sub t(d3t::kSubScan); d3::D3Core::ScanCasterRuns(frame, runs, &cc_->d3c_.c.marker_ordinal_mismatch, !cc_->d3c_.verify, cc_->d3c_.fault); }
    const d3::Hooks h = D3CHooks(frame, capture);
    const std::vector<uint64_t> no_keys;
    const std::vector<thin::ThinKey> no_fulls;
    { d3t::Sub t(d3t::kSubObserve); cc_->d3c_.Observe(frame, runs, no_keys, no_fulls, h); }   // (a caster run takes its own keys: CasterKeys)
    cc_->d3c_.verify_keys.clear();
    { d3t::Sub t(d3t::kSubPublish); cc_->d3c_.Publish(h, frame.header.guest_frame); }
    if (!cc_->d3c_offline_) {
      const uint64_t before = cc_->d3c_.packets->stats().evicted_unconsumed;
      cc_->d3c_.packets->Evict(frame.header.guest_frame);
      cc_->d3c_.c.unconsumed += cc_->d3c_.packets->stats().evicted_unconsumed - before;
    } else {
      const uint64_t ev = cc_->d3c_.packets->stats().evicted_unconsumed;
      cc_->d3c_.c.unconsumed += ev - cc_->d3c_evict_seen_;
      cc_->d3c_evict_seen_ = ev;
    }
    {   // the hook's keyed calls of this frame against the binder's keyed runs (markers included): the census guard of the run grouping [L]
      uint32_t keyed_runs = 0;
      const bool cached_ok = cc_->d3c_.LoadCachedObjects(cc_->d3c_.HookFrame(frame.header.guest_frame));   // lane 301: false = the frame's cached-object list overflowed: not compared
      uint64_t kd = 0;
      for (const d3::Run& r : runs)
        if (r.Keyed()) { kd += r.draws.size(); if (!cc_->d3c_.IsCachedObject(cc_->d3c_.KeyObject(r))) ++keyed_runs; }   // lane 301: a path-4 (replayed list) object records no part in the hook: its runs are left out of calls = runs
      uint32_t markers_here = 0;
      for (uint32_t op = 0; op < frame.ops.count; ++op)
        if (frame.ops[op].kind == OpKind::Draw && frame.OriginOf(frame.ops[op].index) && d3::IsCasterMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site)) ++markers_here;
      uint32_t hook_keyed = 0;
      if (cached_ok && cc_->d3c_.hook_tally && cc_->d3c_.hook_tally->KeyedCalls(cc_->d3c_.HookFrame(frame.header.guest_frame), hook_keyed)) {
        ++cc_->d3c_.c.calls_checked;
        if (hook_keyed != keyed_runs + (cc_->d3c_.verify ? 0u : markers_here)) ++cc_->d3c_.c.calls_differ;
      }
      cc_->d3c_.c.keyed_draws += kd + (cc_->d3c_.c.drawn - cc_->d3c_base_.drawn);
    }
    const uint64_t bad = cc_->d3c_.force_mark ? 0 : (cc_->d3c_.c.holes - cc_->d3c_base_.holes) + (cc_->d3c_.c.no_sequence - cc_->d3c_base_.no_sequence) + (cc_->d3c_.c.proof_mismatch_marked - cc_->d3c_base_.proof_mismatch_marked) +
                                              (cc_->d3c_.c.marker_ordinal_mismatch - cc_->d3c_base_.marker_ordinal_mismatch) + (cc_->d3c_.c.unconsumed - cc_->d3c_base_.unconsumed);
    const uint64_t hole_bad = cc_->d3c_.force_mark ? 0 : (cc_->d3c_.c.holes - cc_->d3c_base_.holes) + (cc_->d3c_.c.no_sequence - cc_->d3c_base_.no_sequence) + (cc_->d3c_.c.marker_ordinal_mismatch - cc_->d3c_base_.marker_ordinal_mismatch) + (cc_->d3c_.c.unconsumed - cc_->d3c_base_.unconsumed);
    cc_->d3c_.gate->Report(true, bad, cc_->d3c_offline_ ? int64_t(frame.header.guest_frame) * 16 : d3::NowMs(), hole_bad);
    cc_->d3c_.c.gate_resets = cc_->d3c_.gate->resets.load(std::memory_order_relaxed);
    if (!cc_->d3c_offline_ && cc_->d3c_.gate->latched.load(std::memory_order_acquire) && !d3c_latched_) {   // lane 228: the arena's latch (review B2) for the caster chain: a systematic hole must not loop skip -> reset -> skip for ever
      d3c_latched_ = true;
      cc_->d3c_.c.latched = 1;
      d3::g_caster_hook_on.store(false, std::memory_order_release);
      char msg[640];
      std::snprintf(msg, sizeof msg, "[rdr][d3][casters] latched off (gate resets %llu: holes %llu, no sequence %llu, marked-object proof mismatches %llu, ordinal mismatches %llu, unconsumed %llu, record missing %llu (a light / world / clock record absent), unsourced after mark %llu, drift after mark %llu); the game records every caster pass again until restart",
                    (unsigned long long)cc_->d3c_.c.gate_resets, (unsigned long long)cc_->d3c_.c.holes, (unsigned long long)cc_->d3c_.c.no_sequence, (unsigned long long)cc_->d3c_.c.proof_mismatch_marked,
                    (unsigned long long)cc_->d3c_.c.marker_ordinal_mismatch, (unsigned long long)cc_->d3c_.c.unconsumed, (unsigned long long)cc_->d3c_.c.record_missing, (unsigned long long)cc_->d3c_.c.unsourced_after_mark, (unsigned long long)cc_->d3c_.c.drift_after_mark);
      d3c_latch_msg_ = msg;
    }
  }
  void D3EndFrame(const LoadedFrame& frame) {
    std::vector<d3::Run> runs;
    const std::function<int(uint64_t)> h_scan_variant = [](uint64_t hash) { return FindVariant(hash); };   // lane 274
    { d3t::Sub t(d3t::kSubScan); d3::D3Core::ScanRuns(frame, runs, &d3_.c.marker_ordinal_mismatch, !d3_.verify, false, d3_.ArenaSplits(), &h_scan_variant); }   // markers count as keyed runs; a marker whose ordinal is not the run count is counted; lane 274: instance calls are separate runs
    d3_.reserved.clear();   // lane 274 (S2): as the ring's: a sweep that binds without the emulation never applies a stale cut list
    const d3::Hooks h = D3Hooks();
    { d3t::Sub t(d3t::kSubObserve); d3_.Observe(frame, runs, thin_keys_, thin_full_, h); }
    d3_.verify_keys.clear();   // lane 204 (Q): this frame's verify markers (BuildMini fills them live)
    { d3t::Sub t(d3t::kSubPublish); d3_.Publish(h, frame.header.guest_frame); }
    if (!d3_offline_) {   // live: packets no marker took are evicted and counted as holes (the hook wrote a packet for a skip that never arrived)
      const uint64_t before = d3_.packets->stats().evicted_unconsumed;
      d3_.packets->Evict(frame.header.guest_frame);
      d3_.c.unconsumed += d3_.packets->stats().evicted_unconsumed - before;
    } else {   // lane 152: offline the emulated hook evicts; its unconsumed packets count the same way (the skipped-frame plant)
      const uint64_t ev = d3_.packets->stats().evicted_unconsumed;
      d3_.c.unconsumed += ev - d3_evict_seen_;
      d3_evict_seen_ = ev;
    }
    {   // lane 152 (17): the hook's keyed calls of this frame against the binder's keyed runs (markers included); (share base) the keyed arena draws seen or synthesized
      const bool ring_claims = d3::g_ring_hook_on.load(std::memory_order_relaxed) && !d3_offline_;   // lane 246: LIVE with the ring hook on, the ring's calls are the ring hook's (d3_ring_hook.h RingScope): the arena's hook never counted them, so neither side of THIS guard does
      const bool cached_ok = d3_.LoadCachedObjects(d3_.HookFrame(frame.header.guest_frame));   // lane 301
      uint32_t keyed_runs = 0;
      uint64_t kd = 0;
      for (const d3::Run& r : runs)
        if (r.Keyed() && !(ring_claims && d3::IsLiveRingModel(r.object))) { kd += r.draws.size(); if (!d3_.IsCachedObject(r.object)) ++keyed_runs; }   // lane 301: path-4 objects are left out of calls = runs
      uint32_t markers_here = 0;
      for (uint32_t op = 0; op < frame.ops.count; ++op)
        if (frame.ops[op].kind == OpKind::Draw && frame.OriginOf(frame.ops[op].index) && (d3::IsMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site) || (!ring_claims && d3::IsRingMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site)))) ++markers_here;   // lane 229: the ring chain's markers stand for runs the arena's hook counted too (offline); lane 246: not when the ring hook claims them live
      uint32_t hook_keyed = 0;
      if (cached_ok && d3_.hook_tally && d3_.hook_tally->KeyedCalls(d3_.HookFrame(frame.header.guest_frame), hook_keyed)) {
        ++d3_.c.calls_checked;
        if (hook_keyed != keyed_runs + (d3_.verify ? 0u : markers_here)) ++d3_.c.calls_differ;
      }
      d3_.c.keyed_draws += kd + (d3_.c.drawn - d3_base_.drawn);
    }
    const uint64_t bad = d3_.force_mark ? 0 : (d3_.c.holes - d3_base_.holes) + (d3_.c.no_sequence - d3_base_.no_sequence) + (d3_.c.proof_mismatch_marked - d3_base_.proof_mismatch_marked) +   // lane 152 (11): a marked object's proof mismatch only
                                              (d3_.c.marker_ordinal_mismatch - d3_base_.marker_ordinal_mismatch) + (d3_.c.unconsumed - d3_base_.unconsumed);
    const uint64_t hole_bad = d3_.force_mark ? 0 : (d3_.c.holes - d3_base_.holes) + (d3_.c.no_sequence - d3_base_.no_sequence) + (d3_.c.marker_ordinal_mismatch - d3_base_.marker_ordinal_mismatch) + (d3_.c.unconsumed - d3_base_.unconsumed);   // the latch counts these only
    d3_.gate->Report(true, bad, d3_offline_ ? int64_t(frame.header.guest_frame) * 16 : d3::NowMs(), hole_bad);   // ONE clock live: the one the hook asks the gate with (review fix 4); offline, frames x 16 ms
    d3_.c.gate_resets = d3_.gate->resets.load(std::memory_order_relaxed);
    if (d3_.gate->latched.load(std::memory_order_acquire) && !d3_latched_) {   // lane 152 (review B2): a systematic hole / mismatch would loop skip -> reset -> skip for ever: D3 is latched off for the session
      d3_latched_ = true;
      d3_.c.latched = 1;
      d3::g_hook_on.store(false, std::memory_order_release);
      char msg[640];
      std::snprintf(msg, sizeof msg, "[rdr][d3] latched off (gate resets %llu: holes %llu, no sequence %llu, marked-object proof mismatches %llu, ordinal mismatches %llu, unconsumed %llu, record missing %llu (a light / world / clock record absent), unsourced after mark %llu, drift after mark %llu); the game records everything again until restart",
                    (unsigned long long)d3_.c.gate_resets, (unsigned long long)d3_.c.holes, (unsigned long long)d3_.c.no_sequence, (unsigned long long)d3_.c.proof_mismatch_marked,
                    (unsigned long long)d3_.c.marker_ordinal_mismatch, (unsigned long long)d3_.c.unconsumed, (unsigned long long)d3_.c.record_missing, (unsigned long long)d3_.c.unsourced_after_mark, (unsigned long long)d3_.c.drift_after_mark);
      d3_latch_msg_ = msg;
    }
  }
  // ---- lane 227 (Wrestlers D3 H2, SKIP_GAME_WORK_MAP.md 18.7): the wrestler class's chain. The arena's functions above are not shared with it (the arena path is byte-for-byte unchanged); the thin store is shared (templates are the D1 store's) ------
  void D3WBeginFrame() {
    wc_->w_pass_ready_ = false;
    wc_->w_.BeginBind();
    wc_->w_base_ = wc_->w_.c;
    ++wc_->w_.c.frames;
  }
  template <class Source>
  d3::Hooks D3WHooks(const LoadedFrame& frame, const Source& capture) {
    d3::Hooks h;
    h.variant = [](uint64_t hash) { return FindVariant(hash); };
    h.pinned = [this](uint64_t key) { return thin_store_.Pinned(key); };
    h.palette_count = [this](uint32_t object, uint32_t part, uint32_t) -> uint32_t { ReplaceSlot* hs = ThinHookSlot(thin::Cls::Wrestlers); return hs && hs->hook ? hs->hook->PartPaletteCount(object, part) : 0u; };   // the file's palette length
    h.slots_used = [this, &capture](const LoadedFrame& f, const Draw& dr) { return D3SlotsUsedWith(wc_->w_slots_cache_, f, dr, capture); };
    (void)frame;
    return h;
  }
  // A wrestler marker: the call's parts are drawn from its sequence (thin draws of the D1 store's templates, as the arena's), the packet and the frame's pass bank.
  template <class Source>
  [[gnu::noinline]] void D3WMarker(const LoadedFrame& frame, uint32_t draw_index, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    d3::WrestlerChain& w = *wc_;
    if (!w.w_pass_ready_) {
      d3::D3Core::PassBank(frame, w.w_pass_vs_, w.w_pass_ps_, w.w_has_vs_, w.w_has_ps_, true);
      w.w_pass_ready_ = true;
    }
    const d3::D3Core::MarkerResult r = w.w_.BuildMini(frame, draw_index, &w.w_pass_vs_, &w.w_pass_ps_, w.w_has_vs_, w.w_has_ps_, w.w_mini_);
    if (r != d3::D3Core::MarkerResult::Ok) {
      ++stats.settings_out;
      return;
    }
    std::swap(thin_keys_, w.w_mini_.keys), std::swap(thin_full_, w.w_mini_.fulls);
    d3_overlay_ = &w.w_mini_.overlay;
    for (uint32_t k = 0; k < w.w_mini_.frame.draws.count; ++k) BindDraw(w.w_mini_.frame, w.w_mini_.frame.draws[k], capture, builder, bytes, stats);
    d3_overlay_ = nullptr;
    std::swap(thin_keys_, w.w_mini_.keys), std::swap(thin_full_, w.w_mini_.fulls);
  }
  template <class Source>
  void D3WEndFrame(const LoadedFrame& frame, const Source& capture) {
    d3::WrestlerChain& w = *wc_;
    d3::D3Core& core = w.w_;
    std::vector<d3::Run> runs;
    d3::D3Core::ScanRuns(frame, runs, &core.c.marker_ordinal_mismatch, !core.verify, true);
    const d3::Hooks h = D3WHooks(frame, capture);
    core.Observe(frame, runs, thin_keys_, thin_full_, h);
    core.verify_keys.clear();
    core.Publish(h, frame.header.guest_frame);
    if (!w.w_offline_) {
      const uint64_t before = core.packets->stats().evicted_unconsumed;
      core.packets->Evict(frame.header.guest_frame);
      core.c.unconsumed += core.packets->stats().evicted_unconsumed - before;
    } else {
      const uint64_t ev = core.packets->stats().evicted_unconsumed;
      core.c.unconsumed += ev - w.w_evict_seen_;
      w.w_evict_seen_ = ev;
    }
    {   // the hook's keyed calls of this frame against the binder's keyed runs (markers included)
      const bool ring_claims = d3::g_ring_hook_on.load(std::memory_order_relaxed) && !w.w_offline_;   // lane 245 (reviewer, as the arena's guard): LIVE with the ring hook on, the ring's calls are the ring hook's (RingScope opens the call; the wrestler scope returns uncounted), so neither side counts them
      uint32_t keyed_runs = 0;
      const bool cached_ok = core.LoadCachedObjects(core.HookFrame(frame.header.guest_frame));   // lane 301
      uint64_t kd = 0;
      for (const d3::Run& r : runs)
        if (r.Keyed() && !(ring_claims && d3::IsLiveRingModel(r.object))) { kd += r.draws.size(); if (!core.IsCachedObject(r.object)) ++keyed_runs; }   // lane 301
      uint32_t markers_here = 0;
      for (uint32_t op = 0; op < frame.ops.count; ++op)
        if (frame.ops[op].kind == OpKind::Draw && frame.OriginOf(frame.ops[op].index) && d3::IsWrestlerMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site)) ++markers_here;
      uint32_t hook_keyed = 0;
      if (cached_ok && core.hook_tally && core.hook_tally->KeyedCalls(core.HookFrame(frame.header.guest_frame), hook_keyed)) {
        ++core.c.calls_checked;
        if (hook_keyed != keyed_runs + (core.verify ? 0u : markers_here)) ++core.c.calls_differ;   // (review S2: no other class's markers are added: the arena and ring chains are never beside this one in a check)
      }
      core.c.keyed_draws += kd + (core.c.drawn - w.w_base_.drawn);
    }
    const uint64_t bad = core.force_mark ? 0 : (core.c.holes - w.w_base_.holes) + (core.c.no_sequence - w.w_base_.no_sequence) + (core.c.proof_mismatch_marked - w.w_base_.proof_mismatch_marked) +
                                               (core.c.marker_ordinal_mismatch - w.w_base_.marker_ordinal_mismatch) + (core.c.unconsumed - w.w_base_.unconsumed);
    const uint64_t hole_bad = core.force_mark ? 0 : (core.c.holes - w.w_base_.holes) + (core.c.no_sequence - w.w_base_.no_sequence) + (core.c.marker_ordinal_mismatch - w.w_base_.marker_ordinal_mismatch) + (core.c.unconsumed - w.w_base_.unconsumed);
    core.gate->Report(true, bad, w.w_offline_ ? int64_t(frame.header.guest_frame) * 16 : d3::NowMs(), hole_bad);
    core.c.gate_resets = core.gate->resets.load(std::memory_order_relaxed);
    if (!w.w_offline_ && !d3w_pick_adopted_) {   // lane 245 (reviewer SF1): the first candidate stamp the census proves (render thread only; the marks are composed from it, the proof compares stay bit-exact)
      const int pick = d3::ChooseSpherePick(core.sphere_reads, core.sphere_eq);   // -1 = undecided (too few reads, or both / neither candidate equal): keep counting
      if (pick >= 0) {
        const bool changed = pick != core.records.sphere_pick;
        core.records.sphere_pick = pick;
        d3w_pick_adopted_ = true;
        d3w_latch_msg_ += std::string(d3w_latch_msg_.empty() ? "" : "\n") + "[rdr][d3][wrestlers] sphere stamp " + (changed ? "adopted" : "confirmed") + ": hook frame " + (d3::kSphereDelta[pick] > 0 ? "+" : "") + std::to_string(d3::kSphereDelta[pick]) + " (" + core.SphereOffsetLine() + ")";
      }
    }
    if (!w.w_offline_ && core.gate->latched.load(std::memory_order_acquire) && !d3w_latched_) {   // lane 245: the arena's latch (review B2) for the wrestler chain: a systematic hole must not loop skip -> reset -> skip for ever
      d3w_latched_ = true;
      core.c.latched = 1;
      d3::g_wrestlers_hook_on.store(false, std::memory_order_release);
      char msg[640];
      std::snprintf(msg, sizeof msg, "[rdr][d3][wrestlers] latched off (gate resets %llu: holes %llu, no sequence %llu, marked-object proof mismatches %llu, ordinal mismatches %llu, unconsumed %llu, record missing %llu (a light / world / clock record absent), unsourced after mark %llu, drift after mark %llu); the game records every wrestler colour call again until restart",
                    (unsigned long long)core.c.gate_resets, (unsigned long long)core.c.holes, (unsigned long long)core.c.no_sequence, (unsigned long long)core.c.proof_mismatch_marked,
                    (unsigned long long)core.c.marker_ordinal_mismatch, (unsigned long long)core.c.unconsumed, (unsigned long long)core.c.record_missing, (unsigned long long)core.c.unsourced_after_mark, (unsigned long long)core.c.drift_after_mark);
      d3w_latch_msg_ = msg;
    }
  }
  std::string D3WReport() const {
    if (!wc_) return "[rdr][d3][wrestlers] not set up";
    const d3::D3Core& core = wc_->w_;
    const d3::D3Counters& k = core.c;
    const d3::CompareStats& r = core.readiness;
    const d3::GuestTally& t = *core.hook_tally;
    char text[2200];
    std::snprintf(text, sizeof text,
                  "[rdr][d3][wrestlers] mode %s: markers %llu, part draws %llu, holes %llu, no sequence %llu, learned %llu, objects complete (marked %llu of %llu), proof objects %llu (parts %llu, mismatching %llu, of marked objects %llu), ordinal mismatch %llu, unconsumed %llu, gate resets %llu, "
                  "calls = runs checked %llu differ %llu, record missing %llu, hook: markers %u dropped %u (gate not proven %u, proof frame %u, table full %u, state overflow %u, unreadable %u), "
                  "readiness: marker parts composed %llu, differing from the stream %llu, register reads %llu, withheld %llu, epoch drops %llu, sequence length differs %llu, never proved %llu, marked with an unsourced register %llu, latched %llu, sink-skipped frames %llu, skipped in proof frames %llu, state differ %llu, drift after mark %llu, unsourced after mark %llu, frozen markers %llu, marked-unsourced peak %llu",
                  d3::ModeName(core.mode), (unsigned long long)k.markers, (unsigned long long)k.drawn, (unsigned long long)k.holes, (unsigned long long)k.no_sequence, (unsigned long long)k.learned, (unsigned long long)core.skippable,
                  (unsigned long long)core.pinned_objects, (unsigned long long)k.proof_objects, (unsigned long long)k.proof_parts, (unsigned long long)k.proof_mismatch_parts, (unsigned long long)k.proof_mismatch_marked,
                  (unsigned long long)k.marker_ordinal_mismatch, (unsigned long long)k.unconsumed, (unsigned long long)k.gate_resets, (unsigned long long)k.calls_checked, (unsigned long long)k.calls_differ, (unsigned long long)k.record_missing,
                  t.markers.load(), t.dropped.load(), t.why[uint8_t(d3::Why::GateNotProven)].load(), t.why[uint8_t(d3::Why::ProofFrame)].load(), t.why[uint8_t(d3::Why::TableFull)].load(), t.why[uint8_t(d3::Why::StateOverflow)].load(),
                  t.why[uint8_t(d3::Why::Unreadable)].load(), (unsigned long long)r.parts, (unsigned long long)r.mismatching, (unsigned long long)r.reads, (unsigned long long)r.withheld, (unsigned long long)core.epoch_drops,
                  (unsigned long long)k.seq_len_differs, (unsigned long long)core.proof_never, (unsigned long long)k.marked_unsourced, (unsigned long long)k.latched, (unsigned long long)d3_skipped_frames_, (unsigned long long)core.skipped_in_proof, (unsigned long long)k.state_differ, (unsigned long long)k.drift_after_mark, (unsigned long long)k.unsourced_after_mark, (unsigned long long)k.frozen_markers, (unsigned long long)k.marked_unsourced_peak);
    std::string out = text;
    if (!core.proof_by_reg.empty()) {
      d3::CompareStats top;
      top.by_reg = core.proof_by_reg;
      out += "\n[rdr][d3][wrestlers] proof mismatch registers (all objects): " + d3::D3Core::TopRegisters(top, 8);
    }
    out += "\n[rdr][d3][wrestlers] readiness top registers: " + d3::D3Core::TopRegisters(r, 8);
    out += "\n[rdr][d3][wrestlers] verify: markers " + std::to_string(k.verify_markers) + ", parts " + std::to_string(k.verify_parts) + ", mismatching " + std::to_string(k.verify_mismatch_parts) + ", unsourced " + std::to_string(k.verify_unsourced_parts) + ", length differs " + std::to_string(k.verify_len_differs);   // lane 245: the verify run's bar (as the casters' review S1)
    if (!wc_->w_offline_) {   // lane 245: the game side's view (the path histogram first: 18.7 precondition 3) and the live switches
      {   // the hook's CUMULATIVE decisions (the per-frame tally above is zeroed by the reader every frame): markers written == the binder's markers (verify: verify markers), and why the others were kept
        const auto cw = [&](d3::Why w) { return std::to_string(t.cum_why[uint8_t(w)].load()); };
        out += "\n[rdr][d3][wrestlers] hook cumulative: calls " + std::to_string(t.cum_calls.load()) + ", markers " + std::to_string(t.cum_markers.load()) + ", dropped " + std::to_string(t.cum_dropped.load()) + " (gate not proven " + cw(d3::Why::GateNotProven) + ", proof frame " + cw(d3::Why::ProofFrame) +
               ", table full " + cw(d3::Why::TableFull) + ", state overflow " + cw(d3::Why::StateOverflow) + ", unreadable " + cw(d3::Why::Unreadable) + ", cache path 2 " + cw(d3::Why::CachePath2) + ", 3 " + cw(d3::Why::CachePath3) + ", 4 " + cw(d3::Why::CachePath4) +
               ", outer cache recording " + cw(d3::Why::OuterRecording) + ")";
      }
      out += "\n" + d3::g_wstats.Line() + "; verify " + (core.verify ? "on" : "off") + ", hook " + (d3::g_wrestlers_hook_on.load(std::memory_order_relaxed) ? "on" : "off") + ", sphere rows indexed " + std::to_string(core.records.sphere_rows());
      out += "\n[rdr][d3][wrestlers] sphere offset census (proof compares of VS c168-c171 against the recorder's row at hook frame - k): " + core.SphereOffsetLine();
    }
    char runs[400];
    std::snprintf(runs, sizeof runs, "\n[rdr][d3][wrestlers] keyed runs seen %llu, refused (stay recorded in full): wrestler program not pinned (cloth, piece, unknown model, not learned yet) %llu, another class's program (arena, props, ring) %llu, no variant %llu, other %llu; records indexed %zu, light cameras %zu, record offset %u, route %s",
                  (unsigned long long)core.wr_runs, (unsigned long long)core.wr_unlearnable[uint8_t(d3::D3Core::CapWhy::NotPinned)], (unsigned long long)core.wr_unlearnable[uint8_t(d3::D3Core::CapWhy::OtherClass)],
                  (unsigned long long)core.wr_unlearnable[uint8_t(d3::D3Core::CapWhy::NoVariant)], (unsigned long long)core.wr_unlearnable[uint8_t(d3::D3Core::CapWhy::Other)], core.records.records(), core.records.wrestler_cameras(), core.record_offset, core.record_route ? "on" : "off");
    out += runs;
    for (const auto& kv : core.reasons) out += "\n[rdr][d3][wrestlers] not marked: " + std::to_string(kv.second) + " objects: " + kv.first;
    return out;
  }
  // lane 152: the record route's census line + the objects not marked, by named reason (one per line)
  std::string D3RecordReportLine() const {
    const d3::D3Counters& k = d3_.c;
    char text[1500];
    std::snprintf(text, sizeof text,
                  "[rdr][d3] record route (mode %s, record offset %u, route %s, verify %s): records indexed %zu (light refs %zu, world entries %zu), light state missing %llu, objects marked %llu of %llu pinned, marked with an unsourced register %llu, never proved %llu, "
                  "proof mismatches of marked objects %llu (all %llu), part draws synthesized %llu of %llu keyed arena draws (%.2f %%), calls = runs checked %llu differ %llu, verify: markers %llu parts %llu mismatching %llu (unsourced %llu), length differs %llu, proof overdue %llu, latched %llu, sink-skipped frames %llu, drift after mark %llu, unsourced after mark %llu, frozen markers %llu, marked-unsourced peak %llu",
                  d3::ModeName(d3_.mode), d3_.record_offset, d3_.record_route ? "on" : "off", d3_.verify ? "on" : "off", d3_.records.records(), d3_.records.light_refs(), d3_.records.world_entries(), (unsigned long long)k.record_missing,
                  (unsigned long long)d3_.skippable, (unsigned long long)d3_.pinned_objects, (unsigned long long)k.marked_unsourced, (unsigned long long)d3_.proof_never, (unsigned long long)k.proof_mismatch_marked,
                  (unsigned long long)k.proof_mismatch_parts, (unsigned long long)k.drawn, (unsigned long long)k.keyed_draws, k.keyed_draws ? 100.0 * double(k.drawn) / double(k.keyed_draws) : 0.0, (unsigned long long)k.calls_checked,
                  (unsigned long long)k.calls_differ, (unsigned long long)k.verify_markers, (unsigned long long)k.verify_parts, (unsigned long long)k.verify_mismatch_parts, (unsigned long long)k.verify_unsourced_parts, (unsigned long long)k.verify_len_differs, (unsigned long long)k.proof_overdue, (unsigned long long)k.latched, (unsigned long long)d3_skipped_frames_, (unsigned long long)k.drift_after_mark, (unsigned long long)k.unsourced_after_mark, (unsigned long long)k.frozen_markers, (unsigned long long)k.marked_unsourced_peak);
    std::string out = text;
    {
      d3::CompareStats top;
      top.by_reg = d3_.proof_by_reg;
      out += "\n[rdr][d3] proof mismatch registers (all objects): " + d3::D3Core::TopRegisters(top, 8);
    }
    for (const auto& kv : d3_.reasons) out += "\n[rdr][d3] not marked: " + std::to_string(kv.second) + " objects: " + kv.first;
    return out;
  }
  std::string D3ReadinessLine() const {
    const d3::CompareStats& r = d3_.readiness;
    char text[900];
    std::snprintf(text, sizeof text,
                  "[rdr][d3] readiness (mode %s): marker parts composed %llu, differing from the stream %llu (%.2f %%), register reads %llu, withheld %llu, differing registers by group: pass %llu template %llu light %llu shadow %llu world %llu clock %llu drift %llu; skipped in proof frames %llu; epoch drops %llu; sequence length differs %llu; top: %s",
                  d3::ModeName(d3_.mode), (unsigned long long)r.parts, (unsigned long long)r.mismatching, r.parts ? 100.0 * double(r.mismatching) / double(r.parts) : 0.0, (unsigned long long)r.reads,
                  (unsigned long long)r.withheld, (unsigned long long)r.by_group[0], (unsigned long long)r.by_group[1], (unsigned long long)r.by_group[2], (unsigned long long)r.by_group[3],
                  (unsigned long long)r.by_group[4], (unsigned long long)r.by_group[5], (unsigned long long)r.by_group[6], (unsigned long long)d3_.skipped_in_proof, (unsigned long long)d3_.epoch_drops, (unsigned long long)d3_.c.seq_len_differs, d3::D3Core::TopRegisters(r, 8).c_str());
    std::string out = text;
    if (r.parts && r.mismatching) {   // the projection: parts whose differing registers all lie in the groups a source would close (light + shadow; + world; + clock; + drift)
      uint64_t only[4] = {};
      const uint32_t l = (1u << uint8_t(d3::Group::Light)) | (1u << uint8_t(d3::Group::Shadow)), w = 1u << uint8_t(d3::Group::World), k = 1u << uint8_t(d3::Group::Clock), dr = 1u << uint8_t(d3::Group::Drift);
      for (const auto& m : r.masks) {
        if ((m.first & ~l) == 0) only[0] += m.second;
        if ((m.first & ~(l | w)) == 0) only[1] += m.second;
        if ((m.first & ~(l | w | k)) == 0) only[2] += m.second;
        if ((m.first & ~(l | w | k | dr)) == 0) only[3] += m.second;
      }
      const double total = double(r.parts);
      char line[600];
      std::snprintf(line, sizeof line,
                    "\n[rdr][d3] readiness projection (parts right once the groups are sourced, of %llu marker parts): light+shadow sourced -> %.2f %%; + world -> %.2f %%; + clock -> %.2f %%; + drift (animated material / mirrored-camera parts) -> %.2f %%; parts right now %.2f %%",
                    (unsigned long long)r.parts, 100.0 * double(r.parts - r.mismatching + only[0]) / total, 100.0 * double(r.parts - r.mismatching + only[1]) / total,
                    100.0 * double(r.parts - r.mismatching + only[2]) / total, 100.0 * double(r.parts - r.mismatching + only[3]) / total, 100.0 * double(r.parts - r.mismatching) / total);
      out += line;
    }
    return out;
  }
  [[gnu::noinline]] void ThinEndFrame() {
    thin_frame_on_ = false;
    if (thin_read_only_) {
      thin_read_only_ = false;
      return;
    }
    if (thin_churn_ && thin_frame_no_ % thin_churn_ == 0) {  // TEST ONLY: the handshake's workout
      std::vector<uint64_t> pinned = thin_store_.PinnedKeys();
      std::sort(pinned.begin(), pinned.end());
      if (!pinned.empty()) {
        const uint64_t victim = pinned[size_t(thin_frame_no_ / thin_churn_) % pinned.size()];
        const thin::ThinStore::Entry* en = thin_store_.Pinned(victim);
        const uint8_t ec = en ? uint8_t(en->t.cls) : uint8_t(thin::Cls::Arena);
        if (thin_store_.Unpin(victim)) ++thin_counters_[ec].unpinned;
      }
    }
    const thin::ThinStore::EndResult r = thin_store_.EndFrame();
    for (thin::ThinCounters& k : thin_counters_) k.released = thin_store_.released();
    if ((++thin_window_ >= 600 || census_flush::Due(g_cf_thin))) {
      thin_window_ = 0;
      LogThinCensus();
    }
  }
  void LogThinCensus() {
    for (uint8_t c = 0; c < uint8_t(thin::Cls::Count); ++c)
      if (ThinClassActive(thin::Cls(c))) rdr::Log("%s", ThinCensusLine(thin::Cls(c)).c_str());
  }
