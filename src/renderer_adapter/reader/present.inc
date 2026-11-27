
// ---- S102 U1 (docs\renderer\SWAP_TAIL.md): OnPresent's end, in functions shared by the old path and the tail latch / finish ----
inline void ResetFrameAndJitter() {
  g_frame.Reset();
  if (g_jitter.on) {  // S10b: the next frame's jitter (this one's phase is in its header)
    g_jitter.phase = (g_jitter.phase + 1) % g_jitter.phases;
    g_jitter.scene_done = false;
    {  // lane 185
      cap::jitter::Window window{g_jitter.hud, g_jitter.legacy, g_jitter.armed};
      cap::jitter::NextFrame(window);
      g_jitter.armed = window.armed, g_jitter.hud = window.hud;
    }
  }
}
inline void PresentSlowFrame(uint64_t guest_frame) {
  {
    // This frame's reader time (the difference since the last present); a burst is logged with its parts.
    const ReaderTimes& n = g_times;
    ReaderTimes& p = g_frame_times;
    const double to_ms = 1.0 / (1e6 * TicksPerNs());  // the time fields are ticks
    const double ms = (n.draw - p.draw) * to_ms;
    uint64_t lk[7];
    TakeStoreLockMax(lk);
    if (ms > kSlowFrameMs) {
      REXLOG_WARN("[rdr][time] slow frame {}: reader {:.1f} ms on the render thread (shaders {:.1f}, targets {:.1f}, state {:.1f}, texture slots {:.1f} "
                  "of which contents {:.1f}, constant banks {:.1f}, geometry {:.1f} of which builds on this thread {:.1f} (gather {:.1f}, key {:.1f}, stored lookup {:.1f}, convert {:.1f}, store {:.1f} with {} sink appends taking {:.1f}, copy + hand to a worker {:.1f}), "
                  "inline builds {}, rebuilt memo entries {}; the slowest single build {:.1f} ms: {} {} bytes {} indices; live store lock held at most: geometry read {:.2f}, append {:.2f}, drop {:.2f}, texture erase {:.2f}, an append waited {:.2f}, an append's allocation {:.2f}, its case-ring copy {:.2f} ms; clock reads only with debug_fine_times)",
                  guest_frame, ms, (n.shaders - p.shaders) * to_ms, (n.targets - p.targets) * to_ms, (n.state - p.state) * to_ms,
                  (n.slots - p.slots) * to_ms, (n.content - p.content) * to_ms, (n.banks - p.banks) * to_ms, (n.geometry - p.geometry) * to_ms,
                  (n.geo_build - p.geo_build) * to_ms, (n.geo_gather - p.geo_gather) * to_ms, (n.geo_key - p.geo_key) * to_ms, (n.geo_find - p.geo_find) * to_ms,
                  (n.geo_conv - p.geo_conv) * to_ms, (n.geo_store - p.geo_store) * to_ms, n.sink_n - p.sink_n, (n.sink - p.sink) * to_ms, (n.geo_hand - p.geo_hand) * to_ms,
                  n.geo_inline - p.geo_inline, n.geo_builds - p.geo_builds, g_worst_build.ticks * to_ms,
                  g_worst_build.worker ? "handed to a worker," : (g_worst_build.is_inline ? "inline," : "indexed,"), g_worst_build.bytes, g_worst_build.count,
                  lk[0] * 1e-6, lk[1] * 1e-6, lk[2] * 1e-6, lk[3] * 1e-6, lk[4] * 1e-6, lk[5] * 1e-6, lk[6] * 1e-6);
    }
    g_worst_build = {};
  }
}
// S102 U1 review F3: the 600-frame census lines (CensusMaybe formats every line of the window) and the cost line; a deferred tail runs them at the next present.
inline void PresentCensus(uint64_t guest_frame) {
  CensusMaybe();
  const auto cost_t0 = std::chrono::steady_clock::now();  // lane 266
  if (const std::string cost_line = wwe2k14::cost::Update(REXCVAR_GET(wwe2k14_cost_counters), guest_frame, REXCVAR_GET(wwe2k14_cost_counters) ? wwe2k14::cost::TicksPerNs() : 1.0); !cost_line.empty())
  {
    REXLOG_INFO("{}", cost_line);  // S101 lane 83: one line every 600 frames (a no-op when the switch is off)
    if (const std::string span_line = wwe2k14::spans::Line(guest_frame); !span_line.empty())
      REXLOG_INFO("{} (totals since start; spans/f over all frames so far)", span_line);  // lane 122: records per span by tag, the half the cost line lacks
    g_periodic.cost.Add(PeriodicMs(cost_t0));  // lane 266
  }
}
inline void PresentCounters(uint64_t guest_frame) {
  PresentSlowFrame(guest_frame);
  PresentCensus(guest_frame);
  g_frame_times = g_times;
}
inline void PresentControl(uint64_t guest_frame) {
  // S101 lane18: labels only for a capture or the case ring, never for a player's live mirror alone. In owner mode the case ring records
  // through the live mirror (g_cap stays false: Check D run 7062 showed labels 0), so the ring's switch is the provenance one (g_prov_on:
  // --wwe2k14_rdr_provenance, --wwe2k14_case, +dev owner, capture frames; lane 17).
  if (rdr_adapter::origin::g_labels_wanted.load(std::memory_order_relaxed) && !g_cap.active &&
      rdr_adapter::origin::Runs().size() >= rdr_adapter::origin::TextRuns::kMaxRuns * 3 / 4)
    rdr_adapter::origin::Runs().Clear();  // S101 lane 89 (review F5): a live session's clocks and counters would fill the 8,192-entry run table and coverage would stop (ids stay valid, they just stop counting)
  rdr_adapter::origin::g_labels.store(g_cap.active || g_prov_on.load(std::memory_order_relaxed) || rdr_adapter::origin::g_labels_wanted.load(std::memory_order_relaxed),  // S101 lane 89: or the ui_text hook is on
                                      std::memory_order_relaxed);
  rdr_adapter::origin::g_on.store(Recording(), std::memory_order_relaxed);  // S101 names: tags only while someone reads them
  g_fine_on = REXCVAR_GET(wwe2k14_rdr_debug_fine_times);
  rdr_adapter::g_store_timing.store(g_fine_on, std::memory_order_relaxed);  // lane 273: the live store's clock reads
  // The bind trace: whole frames, present to present.
  if (const uint32_t left = g_trace_left.load(std::memory_order_relaxed)) {
    g_trace_left.store(left - 1, std::memory_order_relaxed);
    REXLOG_INFO("[rdr][trace] present (guest frame {}){}", guest_frame, left == 1 ? ": trace ended" : "");
    if (left == 1) wwe2k14::nf_observers::g_set_texture.store(nullptr, std::memory_order_relaxed);
  }
  if (const int32_t trace = REXCVAR_GET(wwe2k14_rdr_bind_trace); trace > 0 && !TraceOn()) {
    REXCVAR_SET(wwe2k14_rdr_bind_trace, 0);
    g_trace_draw = 0;
    g_trace_left.store(uint32_t(trace), std::memory_order_relaxed);
    wwe2k14::nf_observers::g_set_texture.store(&OnSetTextureTrace, std::memory_order_relaxed);
    REXLOG_INFO("[rdr][trace] started: {} frame(s) from guest frame {}", trace, guest_frame);
  }
  const int32_t want = REXCVAR_GET(wwe2k14_rdr_capture_frames);
  if (want > 0 && !g_cap.active) {
    REXCVAR_SET(wwe2k14_rdr_capture_frames, 0);
    if (g_live.active()) REXLOG_WARN("[rdr][capture] not started: the live mirror is running (one or the other)");
    else StartCapture(uint32_t(want));
  }
  if (REXCVAR_GET(wwe2k14_rdr_live) && !g_live.active() && !g_cap.active) StartLive();
}

// ---- S102 U1: the tail after the release (switch wwe2k14_rdr_tail_release; tail_release.h has the order and the window) ----
// Swap thread. Latched in OnPresent (g_mutex held), finished by TailFinishAt (the release hook; the walk start / the next present as safety nets).
struct TailPending {
  uint64_t guest_frame = 0;
  bool in_memory = false;
  std::shared_ptr<rdr_adapter::handoff::FrameData> frame;   // the in-memory way: the frame's tables, detached from g_frame at the latch
  rdr_adapter::handoff::FrameData bytes_frame;               // the bytes way: the same, kept (its capacity ping-pongs with g_frame's)
  rdr_scene::live::Latched scene;                            // the scene record's deferred emit (if every contributor latched)
};
inline TailPending g_tail;
// Review F3: the census part of a finished tail (CensusMaybe, the cost line, the memo sweep) runs at the next present, under the reader lock while the update
// thread is parked, not in the finish where the update thread's lock hooks would wait for it. Its frame number is the finished frame's.
inline bool g_tail_census_due = false;
inline uint64_t g_tail_census_frame = 0;
inline uint64_t g_tail_window_start = 0;
inline rdr_adapter::tail::Stats g_tail_window;
inline uint64_t g_tail_violations_seen = 0;

// Every present: may its tail run after the release? (counts the answer by reason; the census line every 600 frames while the switch is on)
inline bool TailDecide(uint64_t guest_frame) {
  namespace tl = rdr_adapter::tail;
  const bool on = rdr_sw::BoolSeen(rdr_sw::kTailRelease, REXCVAR_GET(wwe2k14_rdr_tail_release), guest_frame);
  uint8_t why = tl::kWhyCount;
  if (!on) why = tl::kOff;
  else if (g_cap.active) why = tl::kCapture;
  else if (!g_live.active()) why = tl::kNotLive;
  else if (cap::LiveSink::FrameBytesNeeded()) why = tl::kCaseRing;
  else if (REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 4) why = tl::kLiveSkip;
  tl::State& st = tl::The();
  const bool defer = st.Decide(why);
  if (on && (guest_frame >= g_tail_window_start + 600 || g_tail_window_start == 0 || (census_flush::Due(g_cf_tail) && guest_frame > g_tail_window_start))) {   // S102 CEN1
    if (g_tail_window_start != 0) {
      const rdr_scene::live::LatchTotals lt = rdr_scene::live::GetLatchTotals();
      REXLOG_INFO("{}; scene latches {}, emits deferred {}, not latchable {}{}{}, registry changed {}, VIOLATIONS {}{}{}", st.CensusLine(g_tail_window_start, guest_frame, g_tail_window),
                  lt.latched, lt.deferred, lt.not_latchable, lt.not_latchable_name.empty() ? "" : " (last: ", lt.not_latchable_name.empty() ? "" : lt.not_latchable_name + ")",
                  lt.registry_changed, lt.violations, lt.first_violation.empty() ? "" : " (first: " + lt.first_violation + ")", "");
      if (lt.violations != g_tail_violations_seen) {
        g_tail_violations_seen = lt.violations;
        REXLOG_WARN("[rdr][tail_release] a contributor read game-thread state during a deferred emit ({} so far, first: {}): its latch does not take everything (docs\renderer\SWAP_TAIL.md)",
                    lt.violations, lt.first_violation);
      }
    }
    g_tail_window = st.stats();
    // render-clock: lookup - start of a diagnostic counting window, not game progress
    g_tail_window_start = guest_frame ? guest_frame : 1;
  }
  return defer;
}

inline void TailFinishAt(rdr_adapter::tail::Where where);

// The latch (OnPresent, the update thread parked): what reads game-thread state, and the frame leaves g_frame. Mirrors OnPresent's live path.
inline void TailLatch(uint8_t* base, uint32_t dev, uint64_t guest_frame) {
  namespace tl = rdr_adapter::tail;
  tl::The().SetFinish(&TailFinishAt);
  g_tail.guest_frame = guest_frame;
  wwe2k14::frame_tick::SceneLatch(guest_frame, g_tail.scene);   // S103 P6: g_tail.scene = {}, then LatchFrame when the writer is enabled (moved, unchanged)
  {
    ScopedNs timer(g_times.present);
    LiveSendImages();  // first: a frame must never name an image the sink lacks
    if (std::array<uint8_t, 1024> curve; wwe2k14::frame_tick::TheCurve().TakePending(curve)) g_live.SetDisplayCurve(curve.data());   // S103 P6
    const rdr_adapter::handoff::Way way = rdr_adapter::handoff::Decide(rdr_sw::BoolSeen(rdr_sw::kHandoffMemory, false, guest_frame), cap::LiveSink::FrameBytesNeeded());
    g_tail.in_memory = way == rdr_adapter::handoff::Way::kMemory;
    const uint32_t interval = db::Ld32(base, dev + kDevPresentInterval);
    if (g_tail.in_memory) {
      rdr_adapter::handoff::FramePool().SetKeep(size_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_max_queued))) + 3);   // review F5: + the frame g_tail holds until the finish
      g_tail.frame = FrameHandoff(guest_frame, interval, uint32_t(g_record_frame));
    } else {
      FillFrameData(guest_frame, interval, g_tail.bytes_frame, uint32_t(g_record_frame));
    }
  }
  ResetFrameAndJitter();   // g_frame is the next frame's from here (a late append from another thread goes there, as after today's Reset)
  PresentControl(guest_frame);
  tl::The().SetPending(g_tail.scene.emit_deferred);
}

// The finish: right after the release (the update thread runs), on the swap thread, before the next walk. Review F1: it holds the tail's finish mutex from
// start to end and the frame stays pending until it is done, so a read-back lock / an attire pack on a game thread (FlushIfPending(kAtLock) before the
// reader lock) never draws a part before frame G is handed over: it waits for this, or runs it itself. Lock order: finish -> the scene writer -> g_mutex.
inline void TailFinishAt(rdr_adapter::tail::Where where) {
  rdr_adapter::tail::State& st = rdr_adapter::tail::The();
  std::lock_guard<std::mutex> finish_lock(st.FinishMutex());
  if (!st.Pending()) return;   // another thread finished it while this one waited
  st.FinishStarted();
  // outside g_mutex: the record touches no reader state (the writer and the contributors have their own locks). S103 P6: frame_tick (a no-op unless deferred)
  wwe2k14::frame_tick::SceneEmit(g_tail.guest_frame, g_tail.scene);
  {
    std::lock_guard lock(g_mutex);
    const auto locked_t0 = std::chrono::steady_clock::now();
    const uint64_t guest_frame = g_tail.guest_frame;
    {
      ScopedNs timer(g_times.present);
      const std::string picture = REXCVAR_GET(wwe2k14_rdr_live_picture);
      if (!picture.empty()) REXCVAR_SET(wwe2k14_rdr_live_picture, std::string());
      const bool send = g_live.active();   // false: the mirror stopped between the latch and here (a quit): the frame is dropped, as a stopped sink drops it
      if (!picture.empty() && send) wwe2k14::g_live_pictures_in_flight.fetch_add(1);   // (debug_live_skip & 4 declined the tail: every deferred frame reaches the sink)
      // (with --wwe2k14_rdr_pace_game the wait is now after the release: the game may be one frame further ahead of the render thread; the pictures are the same)
      if (send && REXCVAR_GET(wwe2k14_rdr_pace_game)) g_live.WaitForRoom(size_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_max_queued))), 1000);  // checks only
      if (!send) {
        g_tail.frame.reset();
      } else if (g_tail.in_memory) {
        g_live.SubmitFrameData(std::move(g_tail.frame), guest_frame, picture);
        g_tail.frame.reset();
      } else {
        g_live.SubmitFrame(rdr_adapter::handoff::Serialize(g_tail.bytes_frame), guest_frame, picture);
      }
      if (send) wwe2k14::frame_tick::NoteStreamPost(guest_frame, g_last_present_interval);   // S103 P6: counted / traced only
      LiveForgetGeometry();
    }
    JoinReaderWork();
    g_times.memo_entries = g_geo_memo.size();
    g_times.images = g_reg.images.size();
    if (Recording()) ++g_record_frame;
    PresentSlowFrame(guest_frame);
    g_frame_times = g_times;
    g_tail_census_due = true;   // review F3: CensusMaybe, the cost line and the memo sweep at the next present (PresentTailCensus)
    g_tail_census_frame = guest_frame;
    st.AddLockedNs(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - locked_t0).count()));
  }
  st.FinishDone(where);
}

// Review F3: a finished tail's census part, at the next present (g_mutex held, the update thread parked). The memo sweep's 600-frame test reads g_record_frame,
// one higher than in the finish: the sweep runs once per 600 frames all the same (one frame later).
inline void PresentTailCensus() {
  if (!g_tail_census_due) return;
  g_tail_census_due = false;
  if (Recording()) ForgetStaleMemo();
  PresentCensus(g_tail_census_frame);
}

// 0x82CB37C0 the swap core (r3 device, r4 the texture to show): the end of the frame.
inline void OnPresent(PPCContext& ctx, uint8_t* base) {
  rdr_adapter::tail::The().FlushIfPending(rdr_adapter::tail::kAtPresent);  // S102 U1: a tail still pending (no release since the last present) finishes first
  std::lock_guard lock(g_mutex);
  PresentTailCensus();   // S102 U1 review F3: the last deferred frame's census lines (nothing when the tail did not defer)
  Census& census = g_census;
  ++census.presents;
  const uint32_t dev = ctx.r3.u32, tex = ctx.r4.u32;
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  g_last_present_interval = db::Ld32(base, dev + kDevPresentInterval);
  if (g_on_present) g_on_present();  // settings parity S11 (posts to the UI thread; never blocks)
  if (g_wide.on() && g_wide.scene_done && !g_wide.hud) {  // lane 171: a scene depth copy but no composite marker (pause / replay / photo mode?): the scene window stayed open to the present
    const uint32_t seq_a = db::Ld32(base, 0x839400F8), seq_b = seq_a ? db::Ld32(base, seq_a) : 0, seq = seq_b ? db::Ld32(base, seq_b + 48) : 0;  // the in-game sequence word (ultrawide.h OnFrame: 9 = in game)
    if (seq == 9) {
      static uint64_t no_marker_frames = 0;
      if (++no_marker_frames == 1) REXLOG_INFO("[rdr][wide] in-game frame {} had a scene depth copy but no post chain marker: the scene's draws stayed squeezed to the present (counted; first one logged)", guest_frame);
    }
  }
  if (g_jitter.on && !g_jitter.legacy && g_jitter.scene_done && !g_jitter.hud) {  // lane 185: the DLSS side of lane 171's detector (it runs only with the wide setting): a scene depth copy but no composite marker (pause / replay / photo mode?)
    static uint64_t no_marker_frames = 0;
    ++no_marker_frames;
    if (no_marker_frames == 1 || no_marker_frames == 100 || no_marker_frames == 1000)
      REXLOG_INFO("[rdr][dlss] frame {} had a scene depth copy but no post chain marker ({} such frames so far): DLSS cannot run on it, its jitter stays off and the next frame is unjittered too", guest_frame, no_marker_frames);
  }
  if (g_wide.on()) cap::wide::NextFrame(g_wide, rex::cvar::GetFlagByName("gpu_wide_all") == "true");  // settings parity S5: the next frame's markers (a frame after one without a scene depth copy is a menu); lane 171: wide_rule.h
  if (tex) {
    const uint32_t id = TextureOfObject(base, tex);
    const Image& img = ById(id);
    rdr::PresentOp p;
    p.source.image = rdr::ImageId(id);
    p.source_rect = {0, 0, img.rec.width, img.rec.height};
    g_frame.ops.push_back({cap::OpKind::Present, uint32_t(g_frame.presents.size())});
    g_frame.presents.push_back(p);
  } else {
    Note("present_no_texture");
  }
  rdr::g_fb_fast.store(REXCVAR_GET(wwe2k14_rdr_fb_fast), std::memory_order_relaxed);    // S102 T4: read every frame (the builder latches it in Begin)
  rdr::g_pack_once.store(REXCVAR_GET(wwe2k14_rdr_pack_once), std::memory_order_relaxed);  // S102 T5
  // S101 lane 26: the live scene writer's frame boundary, once per frame, for every sink (the capture folder, the case ring). One relaxed load when off.
  if (g_cap.active) CheckWriter();  // S101 lane 26 review: a writer error / budget stop is seen BEFORE the record is numbered (CheckWriter is safe twice)
  rdr_adapter::origin::g_memo_on.store(rdr_sw::BoolSeen(rdr_sw::kOriginMemo, false, guest_frame), std::memory_order_relaxed);   // S102 T11: read every frame
  {  // S102 T15: the draw memos' switches, read every frame (the hot paths read the relaxed atomics); the census line every 600 frames
    namespace dm = rdr_adapter::slot_geo_memo;
    dm::g_content_skip_on.store(rdr_sw::BoolSeen(rdr_sw::kSlotContentSkip, false, guest_frame), std::memory_order_relaxed);
    dm::g_slot_ways_on.store(rdr_sw::BoolSeen(rdr_sw::kSlotWays, false, guest_frame), std::memory_order_relaxed);
    dm::g_sampler_memo_on.store(rdr_sw::BoolSeen(rdr_sw::kSamplerMemo, false, guest_frame), std::memory_order_relaxed);
    dm::g_geo_hash_once_on.store(rdr_sw::BoolSeen(rdr_sw::kGeoHashOnce, false, guest_frame), std::memory_order_relaxed);
    dm::LogCensus(guest_frame, +[](const char* line) { REXLOG_INFO("{}", line); });
  }
  rdr_adapter::particle_lite::g_on.store(rdr_sw::BoolSeen(rdr_sw::kParticleLite, false, guest_frame), std::memory_order_relaxed);   // S102 T18: read every frame
  rdr_adapter::particle_lite::LogCensus(guest_frame, +[](const char* line) { REXLOG_INFO("{}", line); });
  {  // S102 SW2: wwe2k14_rdr_draw_lite, bit 1: the lite record path for every draw (particle_lite.h, the general gate); bit 2 is unused (R23 cut 8, inline copies in the native constant uploads, measured about 0 and removed)
    namespace pl = rdr_adapter::particle_lite;
    const int64_t m = rdr_sw::IntSeen(rdr_sw::kDrawLite, 0, guest_frame);
    pl::g_gate_gen.general = true, pl::g_gate_gen.census = &pl::g_census_gen;  // (idempotent: the gate's two settings)
    pl::g_on_gen.store((m & 1) != 0, std::memory_order_relaxed);
    pl::LogCensusGeneral(guest_frame, +[](const char* line) { REXLOG_INFO("{}", line); });
  }
  rdr_adapter::draw_memos::g_note_on.store(rdr_sw::BoolSeen(rdr_sw::kNoteMemo, false, guest_frame), std::memory_order_relaxed);   // S102 T14: read every frame
  rdr_adapter::draw_memos::g_shader_on.store(rdr_sw::BoolSeen(rdr_sw::kShaderMemo, false, guest_frame), std::memory_order_relaxed);
  rdr_adapter::draw_memos::g_pin_hash_on.store(rdr_sw::BoolSeen(rdr_sw::kPinHash, false, guest_frame), std::memory_order_relaxed);
  rdr_adapter::origin::g_index_on.store(rdr_sw::BoolSeen(rdr_sw::kOriginIndex, false, guest_frame), std::memory_order_relaxed);   // S102 T14b
  wwe2k14::upd_lean::g_census.store(SourceCensusOn(), std::memory_order_relaxed);   // S102 U2: the notified marks are made only while a census asks for them (read per frame)
  wwe2k14::upd_lean::g_mask.store(uint32_t(rdr_sw::IntSeen(rdr_sw::kUpdLean, 0, guest_frame)) & wwe2k14::upd_lean::kAll2, std::memory_order_relaxed);   // S102 U2: read every frame
  wwe2k14::swap_tail2::g_mask.store(uint32_t(rdr_sw::IntSeen(rdr_sw::kSwapTail2, 0, guest_frame)) & wwe2k14::swap_tail2::kAll, std::memory_order_relaxed);   // S102 R25-A: read every frame
  rdr_adapter::draw_memos::g_shader_serial_on.store(rdr_sw::BoolSeen(rdr_sw::kShaderSerial, false, guest_frame), std::memory_order_relaxed);   // S102 T14b
  if (TailDecide(guest_frame)) {   // S102 U1 (docs\renderer\SWAP_TAIL.md): the latch now, the rest right after the game releases its update thread (TailFinishAt)
    TailLatch(base, dev, guest_frame);
    return;
  }
  // S103 P6: the record's boundary (EndFrame when the writer is enabled, under the S102 H3 split timer) lives in renderer_game\frame_tick.h (moved, unchanged)
  wwe2k14::frame_tick::SceneEnd(guest_frame, g_cap.active && !g_cap.stop ? g_cap.frame_index : rdr_scene::live::kNoCaptureFrame);
  if (g_cap.active) {
    CheckWriter();
    if (!g_cap.stop) {
      WriteImagesTable();  // first: a frame must never name an image the table lacks
      WriteFrameFile(guest_frame, db::Ld32(base, dev + kDevPresentInterval));
      ++g_cap.frame_index;
    }
    CheckWriter();
    if (g_cap.stop || g_cap.frame_index >= g_cap.frames_left) StopCapture(g_cap.stop ? "stopped early" : "all frames written");
  } else if (g_live.active()) {
    ScopedNs timer(g_times.present);
    LiveSendImages();  // first: a frame must never name an image the sink lacks
    if (std::array<uint8_t, 1024> curve; wwe2k14::frame_tick::TheCurve().TakePending(curve)) g_live.SetDisplayCurve(curve.data());   // S103 P6
    const std::string picture = REXCVAR_GET(wwe2k14_rdr_live_picture);
    if (!picture.empty()) REXCVAR_SET(wwe2k14_rdr_live_picture, std::string());
    // Counted until the sink has handled it (picture_done; the route's quit waits for it, src\pending_work.h). A frame left out by the
    // bisection switch below never reaches the sink, so it is not counted.
    if (!picture.empty() && !(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 4)) wwe2k14::g_live_pictures_in_flight.fetch_add(1);
    if (REXCVAR_GET(wwe2k14_rdr_pace_game)) g_live.WaitForRoom(size_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_max_queued))), 1000);  // checks only
    if (!(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 4)) {
      // S102 lane T8: read once per guest frame here (the record side) and carried by the message; the render thread never reads it. The case ring needs bytes.
      bool first_forced = false;
      const rdr_adapter::handoff::Way way = rdr_adapter::handoff::Decide(rdr_sw::BoolSeen(rdr_sw::kHandoffMemory, false, guest_frame), cap::LiveSink::FrameBytesNeeded(), &first_forced);
      if (first_forced) REXLOG_INFO("[rdr][live] handoff_memory is on but the case ring is on (a +dev owner build turns it on; --wwe2k14_case=false stops that): every frame is still serialised, nothing is handed over in memory");
      const bool in_memory = way == rdr_adapter::handoff::Way::kMemory;
      if (in_memory) rdr_adapter::handoff::FramePool().SetKeep(size_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_max_queued))) + 2);
      if (in_memory) g_live.SubmitFrameData(FrameHandoff(guest_frame, db::Ld32(base, dev + kDevPresentInterval), uint32_t(g_record_frame)), guest_frame, picture);
      else g_live.SubmitFrame(FrameBytes(guest_frame, db::Ld32(base, dev + kDevPresentInterval), uint32_t(g_record_frame)), guest_frame, picture);
      wwe2k14::frame_tick::NoteStreamPost(guest_frame, g_last_present_interval);   // S103 P6: counted / traced only
    }
    LiveForgetGeometry();
  }
  JoinReaderWork();  // a frame left out of the sink (a stopped capture, debug_live_skip 4): its jobs still joined before its tables go
  if (Recording()) ForgetStaleMemo();  // after the join: no entry is pending
  g_times.memo_entries = g_geo_memo.size();
  g_times.images = g_reg.images.size();
  if (Recording()) ++g_record_frame;
  ResetFrameAndJitter();   // S102 U1: moved into a function (unchanged), shared with TailLatch
  PresentCounters(guest_frame);   // S102 U1: the slow-frame line, CensusMaybe, the cost line (unchanged; the cost line's frame is this present's guest_frame = the g_guest_frame it read)
  PresentControl(guest_frame);    // S102 U1: the label / origin flags, the bind trace, capture / live start (unchanged)
}

// 0x82CA87C0: called after the original returned the object in r3.
inline void OnTextureCreated(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32;
  if (!obj) return;
  std::unique_lock lock = LockReaderCounted();   // S102 U1 review F3 (counts a wait on a running tail finish)
  ++g_reg.epoch;
  if (auto it = g_reg.tex_by_object.find(obj); it != g_reg.tex_by_object.end()) {
    ById(it->second).live = false;
    g_reg.tex_by_object.erase(it);
  }
  RegisterTextureObject(base, obj);
}

// 0x82CAD760: every kind of resource goes through it; only ours are acted on.
inline void OnResourceReleased(PPCContext& ctx, uint8_t* base) {
  (void)base;
  std::unique_lock lock = LockReaderCounted();   // S102 U1 review F3
  OnReleased(ctx.r3.u32);
}

// 0x82CA6BE0: the CPU says it wrote an object: its contents are read again at the next bind.
// ---- attires packed on the CPU (S100) -----------------------------------------------------------------------------------------------
// CEditExSSCompositTexture (its Create is sub_83419478, named by the game's own assert text) builds an attire on the GPU into an 8888 atlas
// per mip level (copy destinations: Heenan's 2048x1024 0xFE8DD000 and 1024x512 0xFE6DD000), then its pack step sub_83417E20 (listing) does
// per level L: lock the atlas, untile it to a CPU buffer, flush-unlock it (0x82CA6BE0 called from 0x83418050), convert 8888 to DXT1 on the
// CPU (sub_82CFD8F0) into the final texture (object +184 -> its D3D texture), flush-unlock that (called from 0x8341819C, r4 = L). The game
// draws the wrestler with the final texture. With nothing read back (owner mode) the CPU's DXT1 comes from stale memory; with the pack
// known, our renderer copies its own atlas into the final texture: the texture's image takes the atlas's storage (RGBA8, written on the
// GPU) and is sampled through the atlas's channel map; the CPU's DXT1 bytes are never read. No read-back, no frame split, no window rule.
inline constexpr uint32_t kPackSourceFlushReturn = 0x83418054;  // sub_83417E20: after flushing the atlas of level L
inline constexpr uint32_t kPackDestFlushReturn = 0x834181A0;    // sub_83417E20: after flushing level L of the final texture
inline uint32_t g_pack_source = 0;                               // the atlas texture object of the level being packed
inline uint64_t g_packs = 0;

inline void OnAttirePacked(uint8_t* base, uint32_t dest_obj, uint32_t level, uint32_t src_obj) {
  const uint32_t src_id = TextureOfObject(base, src_obj);
  if (!ById(src_id).copy_dest) {
    Note("pack_source_not_a_copy");
    return;
  }
  const uint32_t dest_id = TextureOfObject(base, dest_obj);
  {
    const Image& src = ById(src_id);
    Image& d = ById(dest_id);
    if (level >= d.rec.mips) {
      Note("pack_level_outside_texture");
      return;
    }
    if (!d.packed || d.rec.format != src.rec.format) {
      // The atlas's channel map (AddTextureSlot's copy-destination read of the atlas with its own words): for each fetch component of the
      // atlas, which storage component it reads. The DXT1 conversion keeps the components, so the final texture's component c is the atlas's.
      uint8_t atlas_host[4] = {0, 1, 2, 3};
      const uint32_t bf = BaseFormat(src.game_format);
      if (bf != 6) Note("pack_atlas_format_other", bf);
      const uint32_t sw = (src.header[3] >> 1) & 0xFFF;
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t sel = (sw >> (3 * c)) & 7;
        uint8_t comp = sel <= 3 ? atlas_host[sel] : uint8_t(sel == 5 ? 5 : 4);
        if (src.swap && comp == 0) comp = 2;
        else if (src.swap && comp == 2) comp = 0;
        d.pack_host[c] = comp;
      }
      d.packed = true;
      d.copy_dest = true;
      d.copy_source = src_id;  // S101 lane17
      d.written_in_capture = true;
      d.linear_static = false;
      g_images_dirty = true;
      d.rec.format = src.rec.format;
      d.rec.flags |= cap::kImageCopyDest;
      d.rec.usage = uint8_t(rdr::kUsageSampled);
    }
  }
  const Image& src = ById(src_id);
  const Image& d = ById(dest_id);
  const uint32_t lw = std::max<uint32_t>(d.rec.width >> level, 1u), lh = std::max<uint32_t>(d.rec.height >> level, 1u);
  const uint32_t cw = std::min<uint32_t>(lw, src.rec.width), ch = std::min<uint32_t>(lh, src.rec.height);
  if (cw != lw || ch != lh) Note("pack_level_size_differs");
  rdr::CopyOp op;
  op.source.image = rdr::ImageId(src_id);
  op.dest.image = rdr::ImageId(dest_id);
  op.dest.mip = uint8_t(level);
  op.source_rect = {0, 0, cw, ch};
  op.dest_rect = {0, 0, cw, ch};
  g_frame.ops.push_back({cap::OpKind::Copy, uint32_t(g_frame.copies.size())});
  g_frame.copies.push_back(op);
  cap::CopyExtra extra;
  extra.dest_format = uint8_t(BaseFormat(d.game_format));
  g_frame.copy_extras.push_back(extra);
  const uint64_t n = ++g_packs;
  // Diagnosis (S100 roster check): the packed level 0 as our renderer holds it, written as pack_<frame>_<texture>.png for
  // tools\assets\attire_check.py (compares it with the attires built from the files). Forces this frame part to be drawn.
  if (level == 0 && REXCVAR_GET(wwe2k14_rdr_debug_pack_png) && g_live.active()) {
    const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    LiveSendImages();
    std::vector<uint8_t> rgba;
    uint32_t rw = 0, rh = 0;
    const bool read = g_live.DrawAndRead(FrameBytes(guest_frame, g_last_present_interval, uint32_t(g_record_frame)), guest_frame, dest_id, 0, 0, rgba, rw, rh);
    g_frame.Reset();
    ++g_record_frame;
    if (read) {
      for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
      std::error_code ec;
      const std::filesystem::path dir = CapturesRoot() / "live";
      std::filesystem::create_directories(dir, ec);
      char name[96];
      std::snprintf(name, sizeof(name), "pack_%06llu_%08X.png", static_cast<unsigned long long>(guest_frame), ById(dest_id).header[1] & 0xFFFFF000u);
      rdr::WritePng((dir / name).string().c_str(), rgba.data(), rw, rh);
    }
  }
  if (n <= 32 || (n & (n - 1)) == 0)
    REXLOG_INFO("[rdr][pack] attire level {} ({}x{}, {} mips, format {}) of texture {:08X} taken from atlas {:08X} ({}x{}) on the GPU, frame {} ({} packs)",
                level, lw, lh, d.rec.mips, d.game_format, d.header[1] & 0xFFFFF000u, src.header[1] & 0xFFFFF000u, src.rec.width, src.rec.height,
                wwe2k14::g_guest_frame.load(std::memory_order_relaxed), n);
}

inline void OnUnlockFlush(PPCContext& ctx, uint8_t* base) {
  if (uint32_t(ctx.lr) == kPackDestFlushReturn) rdr_adapter::tail::The().FlushIfPending(rdr_adapter::tail::kAtLock);   // S102 U1 review F1: the pack's copy (and its debug read-back) after frame G
  std::unique_lock lock = LockReaderCounted();
  // S100: the attire pack step's two flushes (OnAttirePacked).
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr == kPackSourceFlushReturn) {
    g_pack_source = ctx.r3.u32;
  } else if (lr == kPackDestFlushReturn && g_pack_source && Recording() && REXCVAR_GET(wwe2k14_rdr_pack_on_gpu)) {
    OnAttirePacked(base, ctx.r3.u32, ctx.r4.u32, g_pack_source);
    g_pack_source = 0;
    return;  // the CPU's DXT1 bytes are not read: no flush mark
  }
  if (auto it = g_reg.tex_by_object.find(ctx.r3.u32); it != g_reg.tex_by_object.end()) {
    Image& img = ById(it->second);
    img.flushed = true;
    img.flush_seen = true;  // S101 lane17
    img.flush_frame = uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
    img.flush_seq = wwe2k14::prov::Map().Stamp(img.flush_frame);  // S101 lane17 review: event order against the page map's writes
    if (img.copy_dest) Note("cpu_flush_on_copy_destination");  // L26
  }
}

// (0x827FB540, the movie planes' flush: OnPlaneFlush, with EnsureContent: strip-360 D8.)

