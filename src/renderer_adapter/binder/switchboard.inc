
  // S101 lane 41: the in-place replace seam. Hooks are asked in registration order (the crowd keeps its own seam, SetCrowdHook, and runs first for its draws);
  // `enabled` is the switch that turns the hook on (g_arena_ours, ...). The hook outlives the binder's use of it.
  void AddReplaceHook(DrawReplaceHook* hook, const bool* enabled) {
    ReplaceSlot slot;
    slot.hook = hook, slot.enabled = enabled;
    replace_slots_.push_back(slot);
  }
  const ReplaceCounters* replace_counters(const char* name) const {
    for (const ReplaceSlot& slot : replace_slots_)
      if (slot.hook && std::strcmp(slot.hook->Name(), name) == 0) return &slot.counters;
    return nullptr;
  }
  // The census line of a hook now (fview prints it at the end; the live path logs it every 600 frames).
  std::string ReplaceCensus(const char* name) const {
    for (const ReplaceSlot& slot : replace_slots_) {
      if (!slot.hook || std::strcmp(slot.hook->Name(), name) != 0) continue;
      const ReplaceCounters& c = slot.counters;
      char line[480];
      char joined_text[96] = "";
      if (c.joined) std::snprintf(joined_text, sizeof joined_text, ", joined with casters %llu", static_cast<unsigned long long>(c.joined));
      if (c.kept[uint8_t(KeepWhy::BudgetDeferred)]) std::snprintf(joined_text + std::strlen(joined_text), sizeof joined_text - std::strlen(joined_text), ", budget deferred %llu", static_cast<unsigned long long>(c.kept[uint8_t(KeepWhy::BudgetDeferred)]));   // S102 T17
      std::snprintf(line, sizeof line, "[rdr][%s] ours frames %llu, fallback %llu, replaced %llu, kept %llu (not ready %llu, unjoined %llu, mismatch %llu, variant %llu, texture %llu, palette %llu, not ours %llu), slots ignored %llu, ms %.2f, persist-only %llu%s",
                    name, static_cast<unsigned long long>(c.ours_frames), static_cast<unsigned long long>(c.fallback_frames), static_cast<unsigned long long>(c.replaced),
                    static_cast<unsigned long long>(Kept(c)), static_cast<unsigned long long>(c.kept[0]), static_cast<unsigned long long>(c.kept[1]), static_cast<unsigned long long>(c.kept[2]),
                    static_cast<unsigned long long>(c.kept[3]), static_cast<unsigned long long>(c.kept[4]), static_cast<unsigned long long>(c.kept[5]), static_cast<unsigned long long>(c.kept[6]), static_cast<unsigned long long>(slot.slots_ignored), c.ms, static_cast<unsigned long long>(c.persist_only), joined_text);
      return line;
    }
    return "";
  }
  // lane 122: D1 thin draws (thin_draw.h). The store publishes into `publisher` (the live pair g_pins by default; fview's offline reader emulation reads the same one).
  // lane 138: D3 skip the object (d3_arena.h / d3_binder.h). fview owns its packet table, mark set and gate (offline); the live binder uses the globals.
  void SetD3Offline(d3::Mode mode, d3::Fault fault, bool force_mark, int lag) {
    d3_.mode = mode, d3_.fault = fault, d3_.force_mark = force_mark, d3_.lag = lag < 0 ? 2 : lag;
    d3_.variant_of = [](uint64_t hash) { return FindVariant(hash); };   // lane 274
    d3_.packets = &d3_own_packets_, d3_.marks = &d3_own_marks_, d3_.gate = &d3_.local_gate, d3_.hook_tally = &d3_.tally;   // lane 152: the (17) census reads the emulated hook's own tally
    d3_own_packets_.Reset(), d3_own_marks_.Reset();
    if (const char* t = std::getenv("D3_TABLE")) d3_own_packets_.SetCapacity(size_t(std::atoi(t)));   // TEST ONLY: a small side table makes the hook's TableFull fallback happen (the fallback check)
    d3::g_arena_d3 = true;
    d3_offline_ = true;
  }
  void SetD3Live(d3::Mode mode) { d3_.mode = mode; }
  // lane 217 (E2b): the caster class's core. route Casters: its own packets, marks and gate (fview owns them offline); the arena's core is untouched.
  void SetD3CasterOffline(d3::Mode mode, d3::Fault fault, bool force_mark, int lag) {
    if (!cc_) cc_ = std::make_unique<d3::CasterChain>();   // (review S1 / N1) self-contained from construction (route Casters, own packets / marks / gate / tally), created only when asked for
    cc_->d3c_.guest.SetMaxCalls(d3::kMaxCasterPlaneCalls);   // lane 290: as g_cobjs (the plane pass's calls 4-11)
    cc_->d3c_.mode = mode, cc_->d3c_.fault = fault, cc_->d3c_.plane_marks_allowed = true, cc_->d3c_.force_mark = force_mark, cc_->d3c_.lag = lag < 0 ? 2 : lag;
    cc_->d3c_own_packets_.Reset(), cc_->d3c_own_marks_.Reset();
    d3::g_caster_d3 = true;
    cc_->d3c_offline_ = true;
  }
  // lane 228 (the live seam, SKIP_GAME_WORK_MAP.md 17.12): the LIVE caster chain. Same core as the offline one (route Casters, Mode::Record: the live default), but its packets / marks / gate / tally are the globals the caster hooks
  // on the game thread use (d3_arena.h g_cpackets / g_cmarks / g_cgate / g_ctally), not the chain's own. Call once, on the render thread, before the first Bind. It does NOT set g_caster_d3 (live.cpp does, with the hook switch).
  void SetD3CasterLive(bool record_route, bool verify, bool plane = false) {   // lane 268: plane = the plane pass is markable live (d3_arena.h g_caster_plane)
    if (!cc_) cc_ = std::make_unique<d3::CasterChain>();
    cc_->d3c_.guest.SetMaxCalls(d3::kMaxCasterPlaneCalls);
    cc_->d3c_.mode = d3::Mode::Record;
    cc_->d3c_.packets = &d3::g_cpackets, cc_->d3c_.marks = &d3::g_cmarks, cc_->d3c_.gate = &d3::g_cgate, cc_->d3c_.hook_tally = &d3::g_ctally;
    cc_->d3c_.record_route = record_route, cc_->d3c_.verify = verify;
    cc_->d3c_.plane_marks_allowed = plane;
    cc_->d3c_.plane_join_fallback = false;   // lane 268 (review S1): a live core composes the plane from the entry c7 only
    cc_->d3c_offline_ = false;
  }
  std::string D3CTakeLatchMessage() { std::string s; s.swap(d3c_latch_msg_); return s; }   // non-empty once, when the caster gate latched the chain off
  // lane 227 (Wrestlers D3 H2): the wrestler class's own chain (d3_binder.h WrestlerChain), the same offline set-up as the casters'
  void SetD3WrestlersOffline(d3::Mode mode, d3::Fault fault, bool force_mark, int lag) {
    if (!wc_) wc_ = std::make_unique<d3::WrestlerChain>();
    wc_->w_.mode = mode, wc_->w_.fault = fault, wc_->w_.force_mark = force_mark, wc_->w_.lag = lag < 0 ? 2 : lag;
    wc_->w_own_packets_.Reset(), wc_->w_own_marks_.Reset();
    d3::g_wrestlers_d3 = true;
    wc_->w_offline_ = true;
  }
  void D3WSetRecordRoute(bool on, bool verify, int record_offset) {
    if (!wc_) wc_ = std::make_unique<d3::WrestlerChain>();
    wc_->w_.record_route = on, wc_->w_.verify = verify;
    if (record_offset >= 0) wc_->w_.record_offset = uint32_t(record_offset);
  }
  // lane 245 (Wrestlers D3, the live seam, SKIP_GAME_WORK_MAP.md 18.9): the LIVE wrestler chain. Same core as the offline one (route Wrestlers, Mode::Record: the live default), but its packets / marks / gate / tally are the globals the wrestler
  // scope on the game thread uses (d3_arena.h g_wpackets / g_wmarks / g_wgate / g_wtally), never the arena's, the ring's or the casters'. Call once, on the render thread, before the first Bind. It does NOT set g_wrestlers_d3 (live.cpp does,
  // with the hook switch).
  void SetD3WrestlersLive(bool record_route, bool verify) {
    if (!wc_) wc_ = std::make_unique<d3::WrestlerChain>();
    wc_->w_.mode = d3::Mode::Record;
    wc_->w_.packets = &d3::g_wpackets, wc_->w_.marks = &d3::g_wmarks, wc_->w_.gate = &d3::g_wgate, wc_->w_.hook_tally = &d3::g_wtally;
    wc_->w_.record_route = record_route, wc_->w_.verify = verify;
    wc_->w_offline_ = false;
  }
  std::string D3WTakeLatchMessage() { std::string s; s.swap(d3w_latch_msg_); return s; }   // non-empty once, when the wrestler gate latched the chain off
  void D3WEmulateReader(LoadedFrame& frame) {
    if (!d3::g_wrestlers_d3 || !wc_) return;
    wc_->w_.EmulateHook(frame);
  }
  bool D3WLoadLedger(const std::string& path) { return wc_ && wc_->w_.LoadLedger(path); }
  bool D3WSaveLedger(const std::string& path) const { return wc_ && wc_->w_.SaveLedger(path); }
  const d3::D3Core* d3w_core() const { return wc_ ? &wc_->w_ : nullptr; }
  void D3CSetRecordRoute(bool on, bool verify, int record_offset) {
    if (!cc_) return;
    cc_->d3c_.record_route = on, cc_->d3c_.verify = verify;
    if (record_offset >= 0) cc_->d3c_.record_offset = uint32_t(record_offset);
  }
  // The caster hooks: the part's own bone count (the palette source needs it in Mode::Record; unset = every palette is NoSrc::NoPalette, stream mode never asks) and the geometry-ready test.
  void D3CSetHooks(std::function<uint32_t(uint32_t, uint32_t, uint32_t)> palette_count, std::function<bool(uint32_t, uint32_t, uint32_t)> geometry) {
    if (!cc_) cc_ = std::make_unique<d3::CasterChain>();
    cc_->d3c_palette_count_ = std::move(palette_count), cc_->d3c_geometry_ = std::move(geometry);
  }
  void D3CEmulateReader(LoadedFrame& frame) {
    if (!d3::g_caster_d3 || !cc_) return;
    cc_->d3c_.EmulateHook(frame);
  }
  bool D3CLoadLedger(const std::string& path) { return cc_ && cc_->d3c_.LoadLedger(path); }
  bool D3CSaveLedger(const std::string& path) const { return cc_ && cc_->d3c_.SaveLedger(path); }
  const d3::D3Core* d3c_core() const { return cc_ ? &cc_->d3c_ : nullptr; }
  // lane 152 (D3-F): the record route. fview adds every record of the capture (offline) and picks the arm; live adds each frame's record (D3AddRecord, live.cpp) and evicts the old ones.
  void D3SetRecordRoute(bool on, bool verify, int record_offset) {
    d3_.record_route = on, d3_.verify = verify;
    if (record_offset >= 0) d3_.record_offset = uint32_t(record_offset);
  }
  void D3AddRecord(const std::shared_ptr<const rdr_scene::SceneTables>& t) {
    d3_.records.Add(t);
    if (d3::g_caster_d3 && cc_) cc_->d3c_.records.Add(t);   // lane 217
    if (d3::g_ring_d3 && rc_) rc_->d3r_.records.Add(t);     // lane 229
    if (d3::g_wrestlers_d3 && wc_) wc_->w_.records.Add(t);   // lane 227
  }
  void D3EvictRecords(uint64_t guest_frame) {
    d3_.records.Evict(guest_frame);
    if (d3::g_ring_d3 && rc_) rc_->d3r_.records.Evict(guest_frame);   // lane 229
    if (d3::g_caster_d3 && cc_) cc_->d3c_.records.Evict(guest_frame);
    if (d3::g_wrestlers_d3 && wc_) wc_->w_.records.Evict(guest_frame);   // lane 227
  }
  // lane 152 (skipped frames): the live sink may SKIP a frame without binding it (a backed-up queue, the 30 fps mode's every other frame). The hook already wrote that frame's packets; no marker will take them, and
  // that is not a hole: they are discarded here, uncounted (an eviction would count them as `unconsumed` and latch D3 off within seconds).
  void D3FrameSkipped(const LoadedFrame& frame) {
    if (!d3::g_arena_d3 && !d3::g_caster_d3 && !d3::g_ring_d3 && !d3::g_wrestlers_d3) return;
    for (uint32_t op = 0; op < frame.ops.count; ++op) {
      if (frame.ops[op].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[op].index);
      if (!o) continue;
      if (d3::g_ring_d3 && rc_ && d3::IsRingMarkerCall(o->call_site)) {   // lane 229: a ring marker's packet is keyed by the model itself, in the ring chain's own table
        (void)rc_->d3r_.packets->Take(frame.draws[frame.ops[op].index].vertex_offset, o->object, o->call_site & 0xFFu);
        continue;
      }
      if (d3::g_arena_d3 && d3::IsMarkerCall(o->call_site)) (void)d3_.packets->Take(frame.draws[frame.ops[op].index].vertex_offset, o->object, o->call_site & 0xFFu);
      else if (d3::g_wrestlers_d3 && wc_ && d3::IsWrestlerMarkerCall(o->call_site)) (void)wc_->w_.packets->Take(frame.draws[frame.ops[op].index].vertex_offset, o->object, o->call_site & 0xFFu);   // lane 227
      else if (d3::g_caster_d3 && cc_ && d3::IsCasterMarkerCall(o->call_site))   // lane 217: a caster marker's packet is keyed by the composite (model, pass)
        (void)cc_->d3c_.packets->Take(frame.draws[frame.ops[op].index].vertex_offset, cc_->d3c_.KeyObject(o->object, d3::CasterPassOfMarker(o->call_site)), o->call_site & 0xFFu);
    }
    ++d3_skipped_frames_;
    if (rc_) ++rc_->d3r_skipped_frames_;   // lane 229
  }
  uint64_t d3_skipped_frames() const { return d3_skipped_frames_; }
  std::string D3TakeLatchMessage() { std::string s; s.swap(d3_latch_msg_); return s; }   // lane 152 (B2): non-empty once, when the gate latched D3 off
  std::string D3RecordReport() const { return D3RecordReportLine(); }
  // The reader's side, emulated for captures (fview): decides each object call as the game's hook would and turns the skipped runs into markers. Call after ThinEmulateReader, before Bind.
  void D3EmulateReader(LoadedFrame& frame) {
    if (!d3::g_arena_d3) return;
    d3_.EmulateHook(frame);
  }
  bool D3LoadLedger(const std::string& path) { return d3_.LoadLedger(path); }
  bool D3SaveLedger(const std::string& path) const { return d3_.SaveLedger(path); }
  std::string D3Census() const {
    const d3::GuestTally& t = d3_.hook_tally ? *d3_.hook_tally : d3_.tally;   // lane 152: live = the hook's own g_tally, offline = the emulated hook's
    char hook[500];   // the hook's tally (d3_guest_core.h GuestTally): what the game side decided. lane 237: CUMULATIVE (cum_*), the per-frame ones are zeroed by the reader every frame
    const auto cw = [&](d3::Why w) { return (unsigned long long)t.cum_why[uint8_t(w)].load(); };
    std::snprintf(hook, sizeof hook, " | hook: calls %llu, markers %llu, dropped %llu (gate not proven %llu, proof frame %llu, table full %llu, state overflow %llu, unreadable %llu, cache path 2 %llu, 3 %llu, 4 %llu); paths: fresh1 %llu, fresh2 %llu, record3 %llu, cached4 %llu, unreadable %llu",
                  (unsigned long long)t.cum_calls.load(), (unsigned long long)t.cum_markers.load(), (unsigned long long)t.cum_dropped.load(), cw(d3::Why::GateNotProven), cw(d3::Why::ProofFrame), cw(d3::Why::TableFull),
                  cw(d3::Why::StateOverflow), cw(d3::Why::Unreadable), cw(d3::Why::CachePath2), cw(d3::Why::CachePath3), cw(d3::Why::CachePath4),
                  (unsigned long long)t.cum_path[0].load(), (unsigned long long)t.cum_path[1].load(), (unsigned long long)t.cum_path[2].load(), (unsigned long long)t.cum_path[3].load(), (unsigned long long)t.cum_path[4].load());
    return d3::CensusLine(d3_.c, d3_.skippable, d3_.pinned_objects) + hook;
  }
  std::string D3Readiness() const { return D3ReadinessLine(); }
  std::string D3WReportText() const { return D3WReport(); }   // lane 227
  // lane 217 (E2b): the caster class's census, readiness and reasons (one block; the numbers the offline acceptance reads).
  std::string D3CReport() const {
    if (!cc_) return "[rdr][d3][casters] not set up";
    const d3::D3Counters& k = cc_->d3c_.c;
    const d3::CompareStats& r = cc_->d3c_.readiness;
    const d3::GuestTally& t = *cc_->d3c_.hook_tally;
    char text[1900];
    std::snprintf(text, sizeof text,
                  "[rdr][d3][casters] mode %s: markers %llu, part draws %llu, holes %llu, no sequence %llu, learned %llu, objects complete (marked %llu of %llu), proof objects %llu (parts %llu, mismatching %llu, of marked objects %llu), ordinal mismatch %llu, unconsumed %llu, gate resets %llu, "
                  "calls = runs checked %llu differ %llu, record missing %llu, hook: markers %u dropped %u (gate not proven %u, proof frame %u, table full %u, state overflow %u, unreadable %u), readiness: marker parts composed %llu, differing from the stream %llu, "
                  "register reads %llu, withheld %llu, epoch drops %llu, sequence length differs %llu, never proved %llu, marked with an unsourced register %llu, latched %llu, sink-skipped frames %llu, skipped in proof frames %llu, drift after mark %llu, unsourced after mark %llu, frozen markers %llu, marked-unsourced peak %llu",
                  d3::ModeName(cc_->d3c_.mode), (unsigned long long)k.markers, (unsigned long long)k.drawn, (unsigned long long)k.holes, (unsigned long long)k.no_sequence, (unsigned long long)k.learned, (unsigned long long)cc_->d3c_.skippable,
                  (unsigned long long)cc_->d3c_.pinned_objects, (unsigned long long)k.proof_objects, (unsigned long long)k.proof_parts, (unsigned long long)k.proof_mismatch_parts, (unsigned long long)k.proof_mismatch_marked,
                  (unsigned long long)k.marker_ordinal_mismatch, (unsigned long long)k.unconsumed, (unsigned long long)k.gate_resets, (unsigned long long)k.calls_checked, (unsigned long long)k.calls_differ, (unsigned long long)k.record_missing,
                  t.markers.load(), t.dropped.load(), t.why[uint8_t(d3::Why::GateNotProven)].load(), t.why[uint8_t(d3::Why::ProofFrame)].load(), t.why[uint8_t(d3::Why::TableFull)].load(), t.why[uint8_t(d3::Why::StateOverflow)].load(),
                  t.why[uint8_t(d3::Why::Unreadable)].load(), (unsigned long long)r.parts, (unsigned long long)r.mismatching, (unsigned long long)r.reads, (unsigned long long)r.withheld, (unsigned long long)cc_->d3c_.epoch_drops,
                  (unsigned long long)k.seq_len_differs, (unsigned long long)cc_->d3c_.proof_never, (unsigned long long)k.marked_unsourced, (unsigned long long)k.latched, (unsigned long long)d3_skipped_frames_, (unsigned long long)cc_->d3c_.skipped_in_proof, (unsigned long long)k.drift_after_mark, (unsigned long long)k.unsourced_after_mark, (unsigned long long)k.frozen_markers, (unsigned long long)k.marked_unsourced_peak);
    std::string out = text;
    {
      d3::CompareStats top;
      top.by_reg = cc_->d3c_.proof_by_reg;
      out += "\n[rdr][d3][casters] proof mismatch registers (all objects): " + d3::D3Core::TopRegisters(top, 8);
    }
    out += "\n[rdr][d3][casters] readiness top registers: " + d3::D3Core::TopRegisters(r, 8);
    out += "\n[rdr][d3][casters] verify: markers " + std::to_string(k.verify_markers) + ", parts " + std::to_string(k.verify_parts) + ", mismatching " + std::to_string(k.verify_mismatch_parts) + ", unsourced " + std::to_string(k.verify_unsourced_parts) + ", length differs " + std::to_string(k.verify_len_differs);   // lane 228 (review S1): the verify run bar
    out += "\n[rdr][d3][casters] runs seen (map/plane/rim): " + std::to_string(cc_->d3c_.caster_runs[1]) + "/" + std::to_string(cc_->d3c_.caster_runs[2]) + "/" + std::to_string(cc_->d3c_.caster_runs[3]) + ", not learnable: " + std::to_string(cc_->d3c_.caster_unlearnable[1]) + "/" +
           std::to_string(cc_->d3c_.caster_unlearnable[2]) + "/" + std::to_string(cc_->d3c_.caster_unlearnable[3]);
    if (!cc_->d3c_offline_) out += "\n[rdr][d3][casters] plane calls without the entry c7 (kCxPlanePos): " + std::to_string(cc_->d3c_.plane_no_entry_pos) + "\n" + d3::g_cstats.Line() + "; verify " + (cc_->d3c_.verify ? "on" : "off") + ", records indexed " + std::to_string(cc_->d3c_.records.records()) + ", hook " + (d3::g_caster_hook_on.load(std::memory_order_relaxed) ? "on" : "off");   // lane 228: the game side's view (live)
    for (const auto& kv : cc_->d3c_.caster_unlearnable_why) out += "\n[rdr][d3][casters] not learnable: " + std::to_string(kv.second) + " runs, first part: " + kv.first;
    for (const auto& kv : cc_->d3c_.reasons) out += "\n[rdr][d3][casters] not marked: " + std::to_string(kv.second) + " objects: " + kv.first;
    return out;
  }
  const d3::D3Core& d3_core() const { return d3_; }
  void SetThinFault(thin::Fault f) { thin_fault_ = f, thin_store_.SetFault(f), thin::g_plant_facts_gate = f == thin::Fault::FactsTableGate, thin::g_plant_no_vb = f == thin::Fault::NoVbFact; }   // lane 129: the plants of reviews F1 / F4 live in the shared hash / predicate
  void SetThinPublisher(thin::PinPublisher* p) { thin_pub_ = p; thin_store_ = thin::ThinStore(p); thin_store_.SetFault(thin_fault_); }
  void SetThinProofK(uint32_t k) { thin_proof_k_ = k; }
  void TrackArenaObjects(std::unordered_set<uint32_t>* set) { thin_arena_objects_ = set; }  // TEST ONLY: the objects the arena hook replaced
  void SetThinCapturedOnly(bool on) { thin_captured_only_ = on; }   // lane 162 (TEST ONLY, fview): the emulation blanks ONLY the draws the capture already carries lite (the live reader's), never a full draw: the reference the hooks' own joins are compared with
  void SetThinOffline(bool on) { thin_offline_ = on; }  // rplay / fview / case zips: a lite draw whose key was not learned yet is "not learned", not a hole
  void TestReleasePart() { if (ReplaceSlot* hs = ThinHookSlot(thin::Cls::Wrestlers)) hs->hook->TestReleaseOne(); }   // TEST ONLY (fview --thin-release-part): the wrestlers' hook destroys one part's buffers
  void TestForceEpoch() { thin_force_epoch_ = true; }  // TEST ONLY (fview --thin-epoch-at): the next thin frame behaves as if the hook dropped its assets
  void SetThinChurn(uint32_t every) { thin_churn_ = every; }   // TEST ONLY: every N-th frame unpins one pinned key (the handshake's workout)
  const thin::ThinCounters& thin_counters(thin::Cls c = thin::Cls::Arena) const { return thin_counters_[uint8_t(c)]; }
  std::string ThinCensus(thin::Cls c = thin::Cls::Arena) const { return ThinCensusLine(c); }  // lane 129 (KC): one line per class
  uint32_t thin_proof_k() const { return thin_proof_k_; }
  std::vector<std::array<uint32_t, 3>> TakeThinNotLearned() { std::vector<std::array<uint32_t, 3>> v; v.swap(thin_not_learned_list_); return v; }   // lane 214
  uint64_t thin_not_learned() const { return thin_not_learned_; }   // lane 143: the census reads the delta over one bound frame (lite draws of a capture whose key was not learned yet)
  // The reader's side, emulated for captures (fview): decides lite per draw from the set published `lag` binds ago and blanks what the lite reader would not fill. Call before Bind.
  void ThinEmulateReader(LoadedFrame& frame, uint32_t lag) {
    if (!thin::AnyOn()) return;  // lane 129 (KC)
    // lane 149: ONE emulation per loaded frame, and its tables live with the frame. fview binds the same LoadedFrame again (the judge's Rebind, a pixel history, a case) after ResetFrame has
    // replayed the frames before it through the same binder: the stamp and the facts table used to be single members of the binder, so the held frame's `thin` / `thin_facts` pointed at the
    // LAST replayed frame's data and a second emulation (draws already blanked) re-keyed its draws from the wrong facts. Check J1's frame 342: bind 1 and 2 whole, bind 3 (the picture) lost the 47
    // arena draws whose facts moved from one frame to the next (the three spotlight beams and the other late additive parts).
    const bool shared = thin_fault_ == thin::Fault::EmuSharedTables;   // PLANT (lane 149): the old behaviour, one EmuFrame for every loaded frame and no "already emulated" test
    EmuFrame& emu = thin_emu_[shared ? nullptr : &frame];
    if (!shared && frame.thin.count == 1 && frame.thin.data == &emu.stamp && emu.stamp.frame == frame.header.guest_frame && (!thin::FactsTableOn() || frame.thin_facts.data == emu.facts.data())) return;
    thin_hist_.push_back(thin_pub_->Acquire());
    while (thin_hist_.size() > size_t(lag) + 1) thin_hist_.erase(thin_hist_.begin());
    std::shared_ptr<const thin::PinSet> use = thin_hist_.size() == size_t(lag) + 1 ? thin_hist_.front() : nullptr;
    thin_lite_.Begin(use, frame.header.guest_frame, thin_proof_k_);
    thin_lite_.SetNoOrdinal(thin_fault_ == thin::Fault::NoOrdinal);
    thin_lite_.SetProofByObject(d3::g_arena_d3);   // lane 138
    thin_lite_.SetWrestlerProofByObject(d3::g_wrestlers_d3);   // lane 227 (H2)
    if (thin::FactsTableOn()) {   // review F1: the SAME predicate the reader fills the table under (a gate that is narrower in the reader than here would hide behind this emulation)
      emu.facts.resize(frame.draws.count);  // the facts of every draw, taken BEFORE a lite draw is blanked (the live reader writes the same table)
      for (uint32_t i = 0; i < frame.draws.count; ++i) emu.facts[i] = ThinFactsOf(frame, i);
      frame.thin_facts = {emu.facts.data(), frame.draws.count};
    }
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const DrawOrigin* o = frame.OriginOf(i);
      if (!o || o->klass != DrawClass::ModelPart || !o->object || (o->pass_site != thin::kSiteColour && o->pass_site != thin::kSiteMirror)) continue;
      // lane 143: a draw the LIVE reader already made lite (a thin capture / case zip: flag kDrawThin, counts 0, a valid layout index) is blanked like the emulation's own, so the hooks' joins (they skip layout >= count) never read its zero counts as a real mesh
      // ("not a ring piece" cached for the whole ring: the ring and its posts were missing from the census redraw of Check J1). It stays lite whether or not the emulated pin set also calls it lite.
      const bool captured_lite = (frame.draws[i].flags & kDrawThin) != 0;
      if (thin_captured_only_ && !captured_lite) continue;   // lane 162 (TEST ONLY): a full draw stays full
      if (!thin_lite_.Query(o->pass_site, o->object, o->part, !(frame.draws[i].flags & kDrawInline), ThinProgramOf(frame, i)) && !captured_lite) continue;
      Draw& d = const_cast<Draw&>(frame.draws[i]);  // (the frame's bytes are the caller's; this is the emulation of a reader that did not build them)
      d.flags = uint8_t((d.flags | kDrawThin) & ~kDrawNoGeometry);
      d.vertex_offset = d.index_offset = thin::kBlankHash;
      d.vertex_bytes = d.index_bytes = d.vertex_count = d.index_count = thin::kBlankCount;
      d.layout = 0xFFFFFFFFu;
    }
    emu.stamp = ThinFrameStamp{thin_lite_.stamp(), 0, frame.header.guest_frame, thin_lite_.lite_draws(), 0};
    frame.thin = {&emu.stamp, 1};
  }
  // S101 lane 67: particle runs as one draw (particle_runs.h). The plant (--particles-fault) and the census (fview / rplay print it at the end).
  void SetParticleFault(particles::Fault fault) { particle_fault_ = fault; }
  void SetBlendKeysWanted(bool wanted) { blend_keys_wanted_ = wanted; }   // S102 R21-A: live.cpp every frame (tween or DLSS on); rplay with a frame pump
  const particles::Census& particle_census() const { return particle_census_; }
  std::string ParticleCensus() const { return particles::CensusLine(particle_census_) + "\n" + particles::BreakdownLine(particle_census_) + "\n" + particles::ReasonsLine(particle_census_); }  // lane 132: + the reasons line
  // S101 lane PO: the post chain's promotion (post_hook.h). The hook outlives the binder's use of it; null = none (the switch does nothing).
  void SetPostHook(PostHook* hook) { post_hook_ = hook; }
  const PostCounters& post_counters() const { return post_; }
  std::string PostCensus() const { return post_hook_ ? post_hook_->Census(post_.draws_dropped, post_.copies_dropped, post_.clears_dropped, post_.targets_dropped) : std::string(); }
  // S101 lane 35: the crowd's first promotion. The hook outlives the binder's use of it; null = none (the switch does nothing).
  void SetCrowdHook(CrowdHook* hook) { crowd_hook_ = hook; }
  // S102 P1: the crowd card bake, owned (bake_hook.h, docs\renderer\CARD_BAKE_OURS.md). The hook outlives the binder's use of it; null = none (the switch does nothing).
  void SetBakeHook(BakeHook* hook) { bake_hook_ = hook; }
  const BakeCounters& bake_counters() const { return bake_; }
  std::string BakeCensus() const {
    char line[640];
    std::snprintf(line, sizeof line, "[rdr][own_card_bake] frames ours %llu, stream's %llu, passes found %llu, passes ours %llu, stream ops taken %llu, draws ours %llu, copies ours %llu, 64x64 passes that are not the bake %llu%s%s",
                  (unsigned long long)bake_.frames_ours, (unsigned long long)bake_.frames_fallback, (unsigned long long)bake_.passes_found, (unsigned long long)bake_.passes_ours, (unsigned long long)bake_.ops_taken,
                  (unsigned long long)bake_.draws_ours, (unsigned long long)bake_.copies_ours, (unsigned long long)bake_.not_a_bake, bake_hook_ ? "; " : "", "");
    std::string text = line;
    if (bake_hook_) text += bake_hook_->Census();
    return text;
  }
  const CrowdCounters& crowd_counters() const { return crowd_; }
  // S102 R21-C: the census line of the crowd's rank work (crowd_rank.h). Strings only here, at print time; the counters it reads are plain integers bumped on the render thread.
  std::string CrowdFastLine() const {
    char b[640];
    const uint64_t dt = crowd_.dens_tests[0] + crowd_.dens_tests[1] + crowd_.dens_tests[2] + crowd_.dens_tests[3];
    std::snprintf(b, sizeof b, "[rdr][crowd_fast] flat %d, stream crowd draws %llu: early-outs (dropped) %llu, kept %llu; density tests %llu (non-origin %llu, before inject %llu, kept %llu, ours absent %llu; culled %llu); rank calls %llu (followed %llu, inherited %llu, fresh %llu), rank thread breaks %llu",
                  g_crowd_rank.Fast() ? 1 : 0, static_cast<unsigned long long>(crowd_.draws_dropped + crowd_.draws_kept), static_cast<unsigned long long>(crowd_.draws_dropped), static_cast<unsigned long long>(crowd_.draws_kept),
                  static_cast<unsigned long long>(dt), static_cast<unsigned long long>(crowd_.dens_tests[0]), static_cast<unsigned long long>(crowd_.dens_tests[1]), static_cast<unsigned long long>(crowd_.dens_tests[2]), static_cast<unsigned long long>(crowd_.dens_tests[3]),
                  static_cast<unsigned long long>(crowd_.dens_cull[0] + crowd_.dens_cull[1] + crowd_.dens_cull[2] + crowd_.dens_cull[3]), static_cast<unsigned long long>(g_crowd_rank.Calls()), static_cast<unsigned long long>(g_crowd_rank.Followed()),
                  static_cast<unsigned long long>(g_crowd_rank.Inherited()), static_cast<unsigned long long>(g_crowd_rank.Fresh()), static_cast<unsigned long long>(g_crowd_rank.ThreadBreaks()));
    return b;
  }
  // S102 P3: the plane chain owned as one pass (plane_pass.h). No hook of its own: the group draws through the plane shadow hook (lane 159).
  const PlaneCounters& plane_counters() const { return plane_; }
  const plane_bake::Counters& plane_bake_counters() const { return pb_; }   // S102 L2
  std::string PlaneBakeCensus() const { return plane_bake::CensusLine(pb_, pb_images_size_); }   // S102 L2
  void PlaneBakeImages(rdr::ImageId& raw, rdr::ImageId& soft, rdr::ImageId& composite) const { raw = pb_raw_, soft = pb_soft_, composite = pb_c_; }   // S102 L2 (fview's dump of the bake)
  std::string PlaneCensus() const {
    char line[1400];
    std::snprintf(line, sizeof line,
                  "[rdr][own_plane] frames with a chain %llu: ours %llu (every mask and blur draw made from the record %llu, some stayed the stream's %llu), the stream's stayed %llu (no hook %llu, irregular chain %llu, role %llu, other %llu, overlap with another planner %llu, G4b blank chain %llu, extra chain-site draws counted %llu); passes %llu, stream ops taken %llu; "
                  "group issued: targets %llu, clears %llu, draws %llu (mask %llu, downscale %llu, blur %llu), copies %llu (+%llu aliased, %llu alias ends); mask + blur draws made by the plane hook %llu, the stream's %llu; dropped-by-another-planner ops issued %llu",
                  (unsigned long long)plane_.frames_chain, (unsigned long long)plane_.frames_ours, (unsigned long long)plane_.frames_complete, (unsigned long long)plane_.frames_partial, (unsigned long long)plane_.frames_declined,
                  (unsigned long long)plane_.declined_no_hook, (unsigned long long)plane_.declined_irregular, (unsigned long long)plane_.declined_role, (unsigned long long)plane_.declined_other, (unsigned long long)plane_.declined_overlap, (unsigned long long)plane_.declined_blank, (unsigned long long)plane_.declined_extra,
                  (unsigned long long)plane_.passes, (unsigned long long)plane_.ops_taken, (unsigned long long)plane_.targets, (unsigned long long)plane_.clears, (unsigned long long)plane_.draws,
                  (unsigned long long)plane_.mask_draws, (unsigned long long)plane_.down_draws, (unsigned long long)plane_.blur_draws, (unsigned long long)plane_.copies, (unsigned long long)plane_.copies_aliased, (unsigned long long)plane_.alias_ended,
                  (unsigned long long)plane_.draws_ours, (unsigned long long)plane_.draws_stream, (unsigned long long)plane_.overlap_issued);
    return line;
  }

  // S101 lane 2 (fview, live reload): a variant's SPIR-V replaced by bytes loaded at run time (the effect rebuilt from its sources). `alpha` empty: the
  // variant keeps its compiled alpha-test twin. Programs made earlier are forgotten; the next Bind uses the new ones. Both empty removes the override.
  // `drop_twin` (S101 fview): the variant has NO alpha-test twin any more (the description says "alpha_test": false), so the compiled one is not used either.
  // Only the programs that use this variant are forgotten (the others keep their ids: the renderer's program table only grows).
  void SetVariantOverride(int variant, std::vector<uint32_t> spirv, std::vector<uint32_t> alpha, bool drop_twin = false) {
    if (spirv.empty() && alpha.empty() && !drop_twin) overrides_.erase(variant);
    else overrides_[variant] = {std::move(spirv), std::move(alpha), drop_twin};
    for (auto it = programs_.begin(); it != programs_.end();) {
      const int vs = int(it->first >> 32), ps = int((it->first >> 1) & 0x7FFFFFFFu) - 1;
      if (vs == variant || ps == variant) it = programs_.erase(it);
      else ++it;
    }
  }
  // Reloaded shaders the driver refused since the last call (cleared by the call).
  std::vector<std::string> TakeOverrideErrors() {
    std::vector<std::string> out;
    out.swap(override_errors_);
    return out;
  }
  const std::unordered_map<uint64_t, uint32_t>& missing() const { return missing_; }
  rdr::PresentOp last_present() const { return last_present_; }
  // lane 201: draws kept after DLSS's input was copied this frame. Photo mode saves DLSS's bare output only when this is 0 (the present shows
  // dlss_out + (final - before_hud); with such draws the bare output would miss them, e.g. the 30 Years filter).
  uint32_t kept_after_dlss_copy() const { return kept_after_dlss_copy_; }
  static int VariantOfHash(uint64_t hash) { return FindVariant(hash); }  // lane 185: the program a vertex shader hash names (rplay --jitter-census classes)
  size_t velocity_draw_count() const { return velocity_draws_.size(); }  // lane 185: this frame's collected wrestler velocity draws (rplay --jitter-census)
  // S101 lane 59: frames whose picture was replaced by the scene's target / frames the stage view was on but found no scene target (the editor's picture stayed).
  uint64_t stage_view_frames() const { return stage_view_frames_; }
  uint64_t stage_view_missed() const { return stage_view_missed_; }
  uint32_t stage_view_target() const { return stage_last_target_; }  // the capture image id presented last (0 = none)
  const std::vector<uint32_t>& gallery() const { return gallery_; }
  rdr::ImageId Mapped(uint32_t id) const { return Map(id); }
  void SetTestFsBlockCapacity(uint32_t bytes) { test_fs_capacity_ = bytes; }   // lane 151 (TEST ONLY)
  void SetTestSlotOverride(uint32_t from, uint32_t to) { test_override_from_ = from, test_override_to_ = to; }   // lane 151 (TEST ONLY): see BindDraw
  // S101 lane 123 (SH2): the census line and counters; the size of our image behind a game image id (0 = the stream's image).
  const shadows_ours::Census& shadows_census() const { return shadows_census_; }
  std::string ShadowsOursReport() const {
    const shadows_ours::Census& c = shadows_census_;
    char text[640];
    std::snprintf(text, sizeof text,
                  "[rdr][shadows_ours] frames %llu (unnamed %llu), maps %u px, plane %u%%, images made %llu (map %llu, plane %llu), draws retargeted %llu, clears %llu, copies whole %llu (partial made whole %llu, duplicates dropped %llu), readers %llu, refused %llu, ms %.2f",
                  static_cast<unsigned long long>(c.frames), static_cast<unsigned long long>(c.frames_unnamed), shadows_ours::MapSize(), g_shadow_plane_percent,
                  static_cast<unsigned long long>(c.images_made), static_cast<unsigned long long>(c.targets_map), static_cast<unsigned long long>(c.targets_plane),
                  static_cast<unsigned long long>(c.draws_retargeted), static_cast<unsigned long long>(c.clears), static_cast<unsigned long long>(c.copies_whole),
                  static_cast<unsigned long long>(c.copies_partial_made_whole), static_cast<unsigned long long>(c.copies_dropped), static_cast<unsigned long long>(c.readers),
                  static_cast<unsigned long long>(c.refused), c.ms);
    return text;
  }
  uint32_t ShadowsOursWidth(uint32_t id) const {
    const ShadowOurs* o = ShadowOursImage(id);
    return o ? o->w : 0;
  }
  uint32_t ShadowsOursHeight(uint32_t id) const {
    const ShadowOurs* o = ShadowOursImage(id);
    return o ? o->h : 0;
  }
  const shadows_ours::Plan& shadows_plan() const { return shadows_plan_; }

 private:
  rdr::ImageId scene_frame_image_ = rdr::kNoImage;   // S103 lane S: SetSceneFrame
  bool skip_2d_rows_ = false;    // S103 L2a: SetSkip2DRows (one Bind)
  std::function<void(rdr::FrameBuilder&)> row2d_insert_;   // S103 L2a review F1: SetSkip2DRows's insert (one Bind)
  rdr::TargetsOp row2d_targets_{};
  bool row2d_have_ = false, row2d_inserted_ = false;
  uint64_t skip_2d_draws_ = 0;   // S103 L2a: stream draws left out by it (total)
  rdr::ImageId scene_frame_depth_ = rdr::kNoImage;
  uint32_t scene_frame_id_ = 0;
  uint64_t scene_frame_unfound_ = 0;
  uint8_t scene_bind_state_ = 0;
  bool fc_remnant_noted_ = false;   // S103 L2b: NoteRemnantOnce's once-per-Bind flag
  bool scene_frame_was_on_ = false;
  rdr::ImageId Map(uint32_t id) const {
    if (scene_frame_id_ && id == scene_frame_id_) return scene_frame_image_;   // S103 lane S: the frame image is our frame (before every other alias)
    if (alias_on_ && id < alias_.size() && alias_[id]) id = alias_[id];  // S101 lane 190: an elided copy's destination reads the source (this frame only)
    if (!ours_.empty()) {  // lane 123 (SH2): a shadow image of ours stands in for the game's (every use: targets, copies, the readers' slots, fview's reads)
      const auto found = ours_.find(id);
      if (found != ours_.end()) return found->second.image;
    }
    return id && id < image_ids_.size() ? image_ids_[id] : rdr::kNoImage;
  }

  void AddToGallery(uint32_t id) {
    if (id && std::find(gallery_.begin(), gallery_.end(), id) == gallery_.end()) gallery_.push_back(id);
  }

  template <class Source>
  static void ListImage(const char* role, uint32_t id, const Source& capture) {
    for (const ImageRecord& record : capture.images()) {
      if (record.id != id) continue;
      rdr::Log("[bind]   %s image %u: %ux%u format %u mips %u kind %u layers %u", role, id, record.width, record.height, uint32_t(record.format), uint32_t(record.mips),
               uint32_t(record.kind), uint32_t(record.layers));
      return;
    }
    rdr::Log("[bind]   %s image %u: not in the table", role, id);
  }

  static int FindVariant(uint64_t hash) {
    const auto* first = std::begin(rdr::fx::kHashes);
    const auto* last = std::end(rdr::fx::kHashes);
    const auto* it = std::lower_bound(first, last, hash, [](const rdr::fx::HashEntry& entry, uint64_t value) { return entry.hash < value; });
    return it != last && it->hash == hash ? int(it->variant) : -1;
  }
