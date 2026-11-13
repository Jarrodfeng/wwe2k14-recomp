// The Binder's Ring D3 members (Session 101, lane 229 = Ring D3 R3; docs\renderer\SKIP_GAME_WORK_MAP.md 16.3 / 16.8). Included INSIDE `class Binder` (binder.h), so it sees the binder's private state; kept in its own file so the
// ring's chain stays one block and the arena's / the casters' functions are not edited. The chain is d3::RingChain (d3_binder.h): D3Core with route Ring, its own packets / marks / gate / tally, created by SetD3RingOffline
// (fview --d3 ring) or SetD3RingLive (lane 246: --wwe2k14_rdr_ring_d3_live; off by default).
//
// The ring is the ARENA's path with another class: the pass bank is the colour draws' camera (PassBank), a part's template and pin come from the thin store's Ring entries (Hooks::pinned), the marker draws its parts as thin-form
// part draws of a mini frame through BindDraw (the arena's D3Marker, with the ring chain's own mini). What differs is in d3_binder.h (route Ring: the marker call, the call-count guard, the slot -> node map) and d3_record.h
// (the ring-rows record). Nothing here touches the arena's or the casters' state.
public:
  // fview --d3 ring: the ring chain's core. Its own packets, marks and gate (fview owns them offline); the arena's core is untouched.
  void SetD3RingOffline(d3::Mode mode, d3::Fault fault, bool force_mark, int lag) {
    if (!rc_) rc_ = std::make_unique<d3::RingChain>();
    rc_->d3r_.mode = mode, rc_->d3r_.fault = fault, rc_->d3r_.force_mark = force_mark, rc_->d3r_.lag = lag < 0 ? 2 : lag;
    rc_->d3r_.records.dedupe_r5 = fault == d3::Fault::RingDedupeModelR5;
    rc_->d3r_.guest.SetMaxCalls(d3::kMaxRingCalls);   // lane 258: the emulated ledger follows up to 16 calls an object, as g_robjs
    rc_->d3r_own_packets_.Reset(), rc_->d3r_own_marks_.Reset();
    if (const char* t = std::getenv("D3_TABLE")) rc_->d3r_own_packets_.SetCapacity(size_t(std::atoi(t)));   // TEST ONLY: a small side table makes the hook's TableFull fallback happen (the fallback check)
    d3::g_ring_d3 = true;
    rc_->d3r_offline_ = true;
    rc_->d3r_.variant_of = [](uint64_t hash) { return FindVariant(hash); };   // lane 274: the unified instance split exempts a wrestler's yCh_ draws
  }
  // lane 246 (the live seam, SKIP_GAME_WORK_MAP.md 16.9): the LIVE ring chain. Same core as the offline one (route Ring, Mode::Record: the live default), but its packets / marks / gate / tally are the ring-only globals the ring hook on the game
  // thread uses (d3_arena.h g_rpackets / g_rmarks / g_rgate / g_rtally), never the arena's (g_packets ...). Call once, on the render thread, before the first Bind. It does NOT set g_ring_d3 or g_ring_hook_on (live.cpp does).
  void SetD3RingLive(bool record_route, bool verify) {
    if (!rc_) rc_ = std::make_unique<d3::RingChain>();
    rc_->d3r_.mode = d3::Mode::Record;
    rc_->d3r_.packets = &d3::g_rpackets, rc_->d3r_.marks = &d3::g_rmarks, rc_->d3r_.gate = &d3::g_rgate, rc_->d3r_.hook_tally = &d3::g_rtally;
    rc_->d3r_.record_route = record_route, rc_->d3r_.verify = verify;
    rc_->d3r_.ordinal_fill_mode = 0, rc_->d3r_.fill_check = false, rc_->d3r_.reserve_ordinals = false;   // lane 289 review S4: the offline env switches (D3_NO_RESERVE / D3_RESERVE_ORACLE / D3_FILL_CHECK) never reach the game: live is always the filled form
    rc_->d3r_offline_ = false;
  }
  std::string D3RTakeLatchMessage() { std::string s; if (rc_) s.swap(rc_->d3r_latch_msg_); return s; }   // non-empty once, when the ring gate latched the chain off (lane 246: public, live.cpp prints it)
  void D3RSetRecordRoute(bool on, bool verify, int record_offset) {
    if (!rc_) return;
    rc_->d3r_.record_route = on, rc_->d3r_.verify = verify;
    if (record_offset >= 0) rc_->d3r_.record_offset = uint32_t(record_offset);
  }
  // The part's own palette size (the file's palette count; unset = the vertices' bone ids decide, Hooks::slots_used).
  void D3RSetHooks(std::function<uint32_t(uint32_t, uint32_t, uint32_t)> palette_count) {
    if (!rc_) rc_ = std::make_unique<d3::RingChain>();
    rc_->d3r_palette_count_ = std::move(palette_count);
  }
  void D3REmulateReader(LoadedFrame& frame) {
    if (!d3::g_ring_d3 || !rc_) return;
    rc_->d3r_.EmulateHook(frame);
  }
  void D3RSetHookFromRecord(bool on) { if (rc_) rc_->d3r_.hook_from_record = on; }   // lane 258: fview --d3-hook-from-record
  bool D3RLoadLedger(const std::string& path) { return rc_ && rc_->d3r_.LoadLedger(path); }
  bool D3RSaveLedger(const std::string& path) const { return rc_ && rc_->d3r_.SaveLedger(path); }
  const d3::D3Core* d3r_core() const { return rc_ ? &rc_->d3r_ : nullptr; }
  // The ring chain's census, readiness and reasons (one block; the numbers the offline acceptance reads). The first line has the casters' shape (d3_caster_check.py's LINE) with the tag [ring].
  std::string D3RReport() const {
    if (!rc_) return "[rdr][d3][ring] not set up";
    const d3::D3Counters& k = rc_->d3r_.c;
    const d3::CompareStats& r = rc_->d3r_.readiness;
    const d3::GuestTally& t = *rc_->d3r_.hook_tally;
    char text[2100];
    std::snprintf(text, sizeof text,
                  "[rdr][d3][ring] mode %s: markers %llu, part draws %llu, holes %llu, no sequence %llu, learned %llu, objects complete (marked %llu of %llu), proof objects %llu (parts %llu, mismatching %llu, of marked objects %llu), ordinal mismatch %llu, unconsumed %llu, gate resets %llu, "
                  "calls = runs checked %llu differ %llu, record missing %llu, hook: markers %u dropped %u (gate not proven %u, proof frame %u, table full %u, state overflow %u, unreadable %u), readiness: marker parts composed %llu, differing from the stream %llu, "
                  "register reads %llu, withheld %llu, epoch drops %llu, sequence length differs %llu, never proved %llu, marked with an unsourced register %llu, latched %llu, sink-skipped frames %llu, skipped in proof frames %llu, drift after mark %llu, unsourced after mark %llu, frozen markers %llu, marked-unsourced peak %llu",
                  d3::ModeName(rc_->d3r_.mode), (unsigned long long)k.markers, (unsigned long long)k.drawn, (unsigned long long)k.holes, (unsigned long long)k.no_sequence, (unsigned long long)k.learned, (unsigned long long)rc_->d3r_.skippable,
                  (unsigned long long)rc_->d3r_.pinned_objects, (unsigned long long)k.proof_objects, (unsigned long long)k.proof_parts, (unsigned long long)k.proof_mismatch_parts, (unsigned long long)k.proof_mismatch_marked,
                  (unsigned long long)k.marker_ordinal_mismatch, (unsigned long long)k.unconsumed, (unsigned long long)k.gate_resets, (unsigned long long)k.calls_checked, (unsigned long long)k.calls_differ, (unsigned long long)k.record_missing,
                  t.markers.load(), t.dropped.load(), t.why[uint8_t(d3::Why::GateNotProven)].load(), t.why[uint8_t(d3::Why::ProofFrame)].load(), t.why[uint8_t(d3::Why::TableFull)].load(), t.why[uint8_t(d3::Why::StateOverflow)].load(),
                  t.why[uint8_t(d3::Why::Unreadable)].load(), (unsigned long long)r.parts, (unsigned long long)r.mismatching, (unsigned long long)r.reads, (unsigned long long)r.withheld, (unsigned long long)rc_->d3r_.epoch_drops,
                  (unsigned long long)k.seq_len_differs, (unsigned long long)rc_->d3r_.proof_never, (unsigned long long)k.marked_unsourced, (unsigned long long)k.latched, (unsigned long long)rc_->d3r_skipped_frames_, (unsigned long long)rc_->d3r_.skipped_in_proof, (unsigned long long)k.drift_after_mark, (unsigned long long)k.unsourced_after_mark, (unsigned long long)k.frozen_markers, (unsigned long long)k.marked_unsourced_peak);
    std::string out = text;
    {
      d3::CompareStats top;
      top.by_reg = rc_->d3r_.proof_by_reg;
      out += "\n[rdr][d3][ring] proof mismatch registers (all objects): " + d3::D3Core::TopRegisters(top, 8);
    }
    out += "\n[rdr][d3][ring] readiness top registers: " + d3::D3Core::TopRegisters(r, 8);
    {
      char line[900];
      std::snprintf(line, sizeof line,
                    "\n[rdr][d3][ring] ring rows: records indexed %zu (light refs %zu, ring entries %zu), ring rows missing %llu, slots learned %llu (unmatched %llu, ambiguous %llu), call-count guard trips %llu, verify: markers %llu parts %llu mismatching %llu (unsourced %llu), length differs %llu, proof overdue %llu, marker unclaimed %llu, palette unknown parts %llu",
                    rc_->d3r_.records.records(), rc_->d3r_.records.light_refs(), rc_->d3r_.records.ring_entries(), (unsigned long long)rc_->d3r_.ring_rows_missing, (unsigned long long)rc_->d3r_.ring_slots_learned,
                    (unsigned long long)rc_->d3r_.ring_slots_unmatched, (unsigned long long)rc_->d3r_.ring_slots_ambiguous, (unsigned long long)rc_->d3r_.guard_trips, (unsigned long long)k.verify_markers, (unsigned long long)k.verify_parts,
                    (unsigned long long)k.verify_mismatch_parts, (unsigned long long)k.verify_unsourced_parts, (unsigned long long)k.verify_len_differs, (unsigned long long)k.proof_overdue, (unsigned long long)k.marker_unclaimed, (unsigned long long)k.palette_unknown_parts);
      out += line;
    }
    {
      char line[300];
      std::snprintf(line, sizeof line, "\n[rdr][d3][ring] proof mismatches by group: pass %llu template %llu light %llu shadow %llu world %llu clock %llu drift %llu, state differ %llu",
                    (unsigned long long)k.by_group[uint8_t(d3::Group::Pass)], (unsigned long long)k.by_group[uint8_t(d3::Group::Template)], (unsigned long long)k.by_group[uint8_t(d3::Group::Light)],
                    (unsigned long long)k.by_group[uint8_t(d3::Group::Shadow)], (unsigned long long)k.by_group[uint8_t(d3::Group::World)], (unsigned long long)k.by_group[uint8_t(d3::Group::Clock)],
                    (unsigned long long)k.by_group[uint8_t(d3::Group::Drift)], (unsigned long long)k.state_differ);
      out += line;
    }
    if (!rc_->d3r_offline_) {   // lane 246: the game side's view (live): the path histogram (the ring TALLY's, lane 237) and the two fresh2 bars, the verify arm and the hook switch
      out += "\n" + d3::g_rstats.Line() + "; verify " + (rc_->d3r_.verify ? "on" : "off") + ", records indexed " + std::to_string(rc_->d3r_.records.records()) + ", hook " + (d3::g_ring_hook_on.load(std::memory_order_relaxed) ? "on" : "off");
      out += "\n[rdr][d3][ring] hook tally (cumulative): calls " + std::to_string(t.cum_calls.load()) + ", markers " + std::to_string(t.cum_markers.load()) + ", dropped " + std::to_string(t.cum_dropped.load()) + " (cache path 2 " +
             std::to_string(t.cum_why[uint8_t(d3::Why::CachePath2)].load()) + ", 3 " + std::to_string(t.cum_why[uint8_t(d3::Why::CachePath3)].load()) + ", 4 " + std::to_string(t.cum_why[uint8_t(d3::Why::CachePath4)].load()) +
             ", outer recording " + std::to_string(t.cum_why[uint8_t(d3::Why::OuterRecording)].load()) + ", unreadable " + std::to_string(t.cum_why[uint8_t(d3::Why::Unreadable)].load()) + ", not markable " +
             std::to_string(t.cum_why[uint8_t(d3::Why::NotMarkable)].load()) + ", proof frame " + std::to_string(t.cum_why[uint8_t(d3::Why::ProofFrame)].load()) + ", gate not proven " +
             std::to_string(t.cum_why[uint8_t(d3::Why::GateNotProven)].load()) + ")";
    }
    out += "\n[rdr][d3][ring] ordinal fill (lane 289): mode " + std::string(rc_->d3r_.ordinal_fill_mode == 0 ? "live" : rc_->d3r_.ordinal_fill_mode == 1 ? "oracle" : "none") + ", generations published " + std::to_string(rc_->d3r_.marks->generation()) + ", binder fills " + std::to_string(rc_->d3r_.fills) + " (missing " + std::to_string(rc_->d3r_.fill_missing) +
           "), oracle check frames " + std::to_string(rc_->d3r_.fill_checked) + " differ " + std::to_string(rc_->d3r_.fill_differ);
    for (const auto& kv : rc_->d3r_.reasons) out += "\n[rdr][d3][ring] not marked: " + std::to_string(kv.second) + " objects: " + kv.first;
    return out;
  }

private:
  // ---- the ring chain's frame (the arena's D3 functions, on the ring chain's own state) ---------------------------------------------------------------------------------------------------------------------
  void D3RBeginFrame() {
    rc_->d3r_pass_ready_ = false;
    rc_->d3r_.BeginBind();
    rc_->d3r_base_ = rc_->d3r_.c;
    ++rc_->d3r_.c.frames;
  }
  // The highest bone id of the draw's vertices + 1 (d3::Hooks::slots_used), cached per (vertex buffer range, layout) in the ring chain's own cache. A ring part's palette size when the file's count is not handed in.
  template <class Source>
  uint32_t D3RSlotsUsed(const LoadedFrame& frame, const Draw& dr, const Source& capture) {
    if (dr.layout >= frame.layouts.count || dr.vertex_bytes == 0) return 0;
    const VertexLayout& vl = frame.layouts[dr.layout];
    if (uint64_t(vl.element_first) + vl.element_count > frame.elements.count || vl.host_stride < 4) return 0;
    int off = -1;
    for (uint32_t k = 0; k < vl.element_count; ++k) {
      const VertexElement& e = frame.elements[vl.element_first + k];
      if (e.source_format == 6 && (e.flags & 2) && e.vk_format == 41) off = e.host_offset;   // the integer 8888 element of VK_FORMAT_R8G8B8A8_UINT = the bone ids (as D3CSlotsUsed)
    }
    if (off < 0 || uint32_t(off) + 4 > vl.host_stride) return 0;
    const uint64_t key = thin::Mix(thin::Mix(thin::Mix(0x51076, dr.vertex_offset), dr.vertex_bytes), (uint64_t(vl.host_stride) << 16) | uint32_t(off));
    auto it = rc_->d3r_slots_cache_.find(key);
    if (it != rc_->d3r_slots_cache_.end()) return it->second;
    std::vector<uint8_t> vb;
    uint32_t used = 0;
    if (capture.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vb) && vb.size() >= dr.vertex_bytes) {
      uint32_t mx = 0;
      for (size_t at = size_t(off); at + 4 <= vb.size(); at += vl.host_stride)
        for (int k = 0; k < 4; ++k)
          if (vb[at + k] != 255) mx = std::max<uint32_t>(mx, vb[at + k]);   // 255 = no influence
      used = mx + 1;
    }
    if (used == 0) return 0;   // a failed read is not cached
    if (rc_->d3r_slots_cache_.size() > 4096) rc_->d3r_slots_cache_.clear();
    rc_->d3r_slots_cache_[key] = used;
    return used;
  }
  template <class Source>
  d3::Hooks D3RHooks(const LoadedFrame& frame, const Source& capture) {
    d3::Hooks h;
    h.variant = [](uint64_t hash) { return FindVariant(hash); };
    h.pinned = [this](uint64_t key) { return thin_store_.Pinned(key); };   // the thin store's Ring entries (Capture's gate checks the entry's class)
    h.palette_count = rc_->d3r_palette_count_;
    h.slots_used = [this, &capture](const LoadedFrame& f, const Draw& dr) { return D3RSlotsUsed(f, dr, capture); };
    (void)frame;
    return h;
  }
  // A ring marker: the object's parts are drawn from its sequence, the packet and the pass bank, through BindDraw as thin draws of a mini frame (the arena's D3Marker on the ring chain's state).
  template <class Source>
  [[gnu::noinline]] void D3RMarker(const LoadedFrame& frame, uint32_t draw_index, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    if (!rc_->d3r_pass_ready_) {
      d3::D3Core::PassBank(frame, rc_->d3r_pass_vs_, rc_->d3r_pass_ps_, rc_->d3r_has_vs_, rc_->d3r_has_ps_);
      rc_->d3r_pass_ready_ = true;
    }
    const d3::D3Core::MarkerResult r = rc_->d3r_.BuildMini(frame, draw_index, &rc_->d3r_pass_vs_, &rc_->d3r_pass_ps_, rc_->d3r_has_vs_, rc_->d3r_has_ps_, rc_->d3r_mini_);
    if (r != d3::D3Core::MarkerResult::Ok) {
      ++stats.settings_out;   // (a hole / no sequence / an epoch drop: counted by BuildMini; the marker draws nothing)
      return;
    }
    std::swap(thin_keys_, rc_->d3r_mini_.keys), std::swap(thin_full_, rc_->d3r_mini_.fulls);
    d3_overlay_ = &rc_->d3r_mini_.overlay;
    for (uint32_t k = 0; k < rc_->d3r_mini_.frame.draws.count; ++k) BindDraw(rc_->d3r_mini_.frame, rc_->d3r_mini_.frame.draws[k], capture, builder, bytes, stats);
    d3_overlay_ = nullptr;
    std::swap(thin_keys_, rc_->d3r_mini_.keys), std::swap(thin_full_, rc_->d3r_mini_.fulls);
  }
  template <class Source>
  void D3REndFrame(const LoadedFrame& frame, const Source& capture) {
    std::vector<d3::Run> runs;
    const std::function<int(uint64_t)> scan_variant = [](uint64_t hash) { return FindVariant(hash); };   // lane 274
    d3::D3Core::ScanRuns(frame, runs, &rc_->d3r_.c.marker_ordinal_mismatch, !rc_->d3r_.verify, false, rc_->d3r_.RingSplits(), &scan_variant);   // lane 258: one run per instance. markers count as keyed runs; a marker whose ordinal is not the run count is counted
    rc_->d3r_.reserved.clear();   // lane 258: a sweep that binds without the emulation never applies a stale cut list
    const d3::Hooks h = D3RHooks(frame, capture);
    rc_->d3r_.Observe(frame, runs, thin_keys_, thin_full_, h);
    rc_->d3r_.verify_keys.clear();
    rc_->d3r_.Publish(h, frame.header.guest_frame);
    if (!rc_->d3r_offline_) {
      const uint64_t before = rc_->d3r_.packets->stats().evicted_unconsumed;
      rc_->d3r_.packets->Evict(frame.header.guest_frame);
      rc_->d3r_.c.unconsumed += rc_->d3r_.packets->stats().evicted_unconsumed - before;
    } else {
      const uint64_t ev = rc_->d3r_.packets->stats().evicted_unconsumed;
      rc_->d3r_.c.unconsumed += ev - rc_->d3r_evict_seen_;
      rc_->d3r_evict_seen_ = ev;
    }
    {   // the hook's keyed calls of this frame against the binder's keyed runs (markers of either class included: both chains' markers stand for runs)
      const bool from_rec = rc_->d3r_.hook_from_record;   // lane 258: offline too, only the models the records' ring-rows entries name are counted (the real hook's calls)
      const bool ring_only = !rc_->d3r_offline_ || from_rec;   // lane 246: LIVE, the ring hook counts only the ring's calls (the class table at [0x836F4C74]); this side counts only the runs of the same models (d3::IsLiveRingModel: the table as the hook last published it)
      uint32_t keyed_runs = 0;
      uint64_t kd = 0;
      for (const d3::Run& r : runs)
        if (r.Keyed() && (!ring_only || (from_rec ? rc_->d3r_.hook_ring_objects.count(r.object) != 0 : d3::IsLiveRingModel(r.object)))) ++keyed_runs, kd += r.draws.size();
      uint32_t markers_here = 0;
      for (uint32_t op = 0; op < frame.ops.count; ++op)
        if (frame.ops[op].kind == OpKind::Draw && frame.OriginOf(frame.ops[op].index) && d3::IsRingMarkerCall(frame.OriginOf(frame.ops[op].index)->call_site)) ++markers_here;   // only the ring chain's own: its emulated hook ran AFTER the arena's, so it saw the arena's markers as non-runs (the arena's guard counts both kinds: it ran first)
      uint32_t hook_keyed = 0;
      if (rc_->d3r_.hook_tally && rc_->d3r_.hook_tally->KeyedCalls(rc_->d3r_.HookFrame(frame.header.guest_frame), hook_keyed)) {
        ++rc_->d3r_.c.calls_checked;
        if (hook_keyed != keyed_runs + (rc_->d3r_.verify ? 0u : markers_here)) ++rc_->d3r_.c.calls_differ;
        if (d3::DebugOn() && hook_keyed != keyed_runs + (rc_->d3r_.verify ? 0u : markers_here)) std::fprintf(stderr, "[d3dbg] ring calls differ frame %llu: hook %u, keyed runs %u + markers %u\n", (unsigned long long)frame.header.guest_frame, hook_keyed, keyed_runs, markers_here);
      }
      rc_->d3r_.c.keyed_draws += kd + (rc_->d3r_.c.drawn - rc_->d3r_base_.drawn);
    }
    const d3::D3Core& dc = rc_->d3r_;
    const d3::D3Counters& b = rc_->d3r_base_;
    uint64_t fill_bad = rc_->d3r_.fill_missing_frame;   // lane 289 review S3: a marker whose part triples the binder could not find (a missing set / key shifts the ordinals of the instances after it) is a hole-type cause; so is a miss the READER counted (live)
    rc_->d3r_.fill_missing_frame = 0;
    if (!rc_->d3r_offline_) {
      const uint64_t rm = d3::g_ring_fill_missing.load(std::memory_order_relaxed);
      fill_bad += rm - rc_->d3r_.reader_missing_seen;
      rc_->d3r_.reader_missing_seen = rm;
    }
    const uint64_t bad = dc.force_mark ? 0 : (dc.c.holes - b.holes) + (dc.c.no_sequence - b.no_sequence) + (dc.c.proof_mismatch_marked - b.proof_mismatch_marked) + (dc.c.marker_ordinal_mismatch - b.marker_ordinal_mismatch) + (dc.c.unconsumed - b.unconsumed) + fill_bad;
    const uint64_t hole_bad = dc.force_mark ? 0 : (dc.c.holes - b.holes) + (dc.c.no_sequence - b.no_sequence) + (dc.c.marker_ordinal_mismatch - b.marker_ordinal_mismatch) + (dc.c.unconsumed - b.unconsumed) + fill_bad;
    rc_->d3r_.gate->Report(true, bad, rc_->d3r_offline_ ? int64_t(frame.header.guest_frame) * 16 : d3::NowMs(), hole_bad);
    rc_->d3r_.c.gate_resets = rc_->d3r_.gate->resets.load(std::memory_order_relaxed);
    if (rc_->d3r_.gate->latched.load(std::memory_order_acquire) && !rc_->d3r_.c.latched) {   // a systematic hole / mismatch would loop skip -> reset -> skip for ever: the ring chain is latched off. It does NOT touch the arena's g_hook_on (isolation); lane 246: the LIVE ring hook is switched off here (g_ring_hook_on, below)
      rc_->d3r_.c.latched = 1;
      if (!rc_->d3r_offline_) d3::g_ring_hook_on.store(false, std::memory_order_release);   // lane 246: the live ring hook stops (the arena's g_hook_on is untouched)
      char msg[640];
      std::snprintf(msg, sizeof msg, "[rdr][d3][ring] latched off (gate resets %llu: holes %llu, no sequence %llu, marked-object proof mismatches %llu, ordinal mismatches %llu, unconsumed %llu, record missing %llu (a light / world / clock record absent), unsourced after mark %llu, drift after mark %llu); the game records every ring call again until restart", (unsigned long long)rc_->d3r_.c.gate_resets,
                    (unsigned long long)rc_->d3r_.c.holes, (unsigned long long)rc_->d3r_.c.no_sequence, (unsigned long long)rc_->d3r_.c.proof_mismatch_marked, (unsigned long long)rc_->d3r_.c.marker_ordinal_mismatch, (unsigned long long)rc_->d3r_.c.unconsumed, (unsigned long long)rc_->d3r_.c.record_missing, (unsigned long long)rc_->d3r_.c.unsourced_after_mark, (unsigned long long)rc_->d3r_.c.drift_after_mark);
      rc_->d3r_latch_msg_ = msg;
    }
  }

