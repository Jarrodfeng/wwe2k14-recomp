// S103 lane H: the scene skips' decision wiring (src\renderer_game\scene_skip_core.h on scene_frame.h / node_switch.h), GPU-free, no game.
// Built and run by tools\renderer\scene_skip_unit.py. `scene_skip_unit.exe PLANT` runs one planted fault; the process must exit 1 (the check CAUGHT it); no argument = the real checks, exit 0.
//   1 hook_decides_alone    a hook calls scene_frame::DecideNow itself (no board): a readiness flip inside a frame makes two hooks disagree
//   2 clock_off_by_one      a hook reads the frame clock + 1: its stamp is for another frame than the one the walks are in
//   3 mode_zero_skips       the hook ignores the mode: mode 0 skips
//   4 hold_ignored          the hook answers Ours inside a hold (the render side reported a miss)
//   5 thread_cache_stale    the per-thread answer is not keyed on the frame: frame j+1 reads frame j's answer
//   7 clk_rule            (S103 CLK2) the model-class hook asks with CLK's rule (g - 1 before latch(g)): its walks of period g are skipped by decision g - 1, the render side draws stream g + 1 from decision g
//   9 render_skip_hole    (S103 CLK2 H2) the render side drops a decided stream (a render-side skip / NoRecord): the stream census must show skip-only
//  10 stream_never_paired (S103 CLK2 H2) a decided stream no NoteStream ever reached: ours_unpaired must exceed the in-flight 1
//  11 ring_model_class    (S103 CLK2 H1) the ring ask is mapped to g (a wrong phase): its stream disagrees with the decision DrawOurs draws from
//   8 lag1_latch_inputs   (S103 CLK2) the latch-stamped inputs are registered with lag 1: the first decider of frame f sees latch(f - 1)'s stamps (labelled f - 2): every bit missing (run 37's signature)
//   6 two_stamps            each hook decides again (no memo): the board holds more than one stamp for one frame
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "../../src/renderer_game/scene_skip_core.h"

using namespace scene_skip;

static int g_fail = 0;
static int g_plant = 0;
static void Check(bool ok, const char* name, const char* what) {
  std::printf("%s  %s: %s\n", ok ? "ok  " : "FAIL", name, what);
  if (!ok) ++g_fail;
}

static std::atomic<uint64_t> g_frame{1};   // the fake frame clock
static int g_mode = 0;                       // the fake cvar storage
static thread_local int t_bias = 0;   // S103 CLK2 (review L2): a walk-time hook is asked with the clock at g + 1 and must answer the same as a model-class hook at g
static uint64_t FakeClock() { return g_frame.load() + uint64_t(t_bias); }

static void ResetAll() {
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  s.ready.store(false), s.required_inputs.store(0), s.hold_until.store(0), s.miss_hold_until.store(0), s.latched.store(false);
  for (auto& f : s.input_frame) f.store(0);
  for (auto& f : s.strike_frames) f.store(0);
  s.strike_next.store(0), s.ours.store(0), s.walks.store(0), s.misses.store(0), s.holes.store(0), s.doubles.store(0);
  for (auto& l : scene_frame::g_input_lag) l.store(0);
  for (int i = 0; i < kHookCount; ++i) g_counts.ours[i].store(0), g_counts.walks[i].store(0), g_counts.clock_swap[i].store(0), g_counts.clock_other[i].store(0);   // S103 CLK2: the clock census too
  t_answer = ThreadAnswer{};
  scene_frame::ResetStreamCensus();   // S103 CLK2: the stream census (slots + counters)
}

// The hook's test with the planted faults applied.
static bool Q(Hook h) {
  if (g_plant == 3) {   // the mode is ignored
    const uint64_t frame = Frame();
    return scene_frame::Decide(frame).decision == uint8_t(rdr_own::Decision::Ours);
  }
  if (g_plant == 1) {   // decides alone, per call, no board
    if (scene_frame::CurrentMode() != scene_frame::Mode::Ours) return false;
    return scene_frame::DecideNow(Frame()).decision == uint8_t(rdr_own::Decision::Ours);
  }
  if (g_plant == 2) {   // the clock + 1
    if (scene_frame::CurrentMode() != scene_frame::Mode::Ours) return false;
    return scene_frame::Ours(Frame() + 1);
  }
  if (g_plant == 4) {   // a hold is not honoured: ask the decision without the hold test
    if (scene_frame::CurrentMode() != scene_frame::Mode::Ours) return false;
    const uint64_t frame = Frame();
    const rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
    return s.ready.load() && scene_frame::InputsMask(frame) == s.required_inputs.load();
  }
  if (g_plant == 5) {   // the per-thread answer is not keyed on the frame
    if (scene_frame::CurrentMode() != scene_frame::Mode::Ours) return false;
    static thread_local bool have = false, ours = false;
    if (!have) have = true, ours = scene_frame::Ours(Frame());
    return ours;
  }
  if (g_plant == 6) {   // no memo: a second stamp for the same frame is written by every hook
    if (scene_frame::CurrentMode() != scene_frame::Mode::Ours) return false;
    const rdr_own::OwnedPassStamp st = scene_frame::DecideNow(Frame());
    rdr_own::g_board.Put(st);
    return st.decision == uint8_t(rdr_own::Decision::Ours);
  }
  return Ours(h);
}

static void Arm(int bits) {   // register `bits` inputs (bit 0..bits-1), lag 0, and mark them captured for the current frame
  for (int b = 0; b < bits; ++b) scene_frame::RequireInputLag(b, 0);
}
static void Mark(int bits, uint64_t frame) {
  for (int b = 0; b < bits; ++b) rdr_own::InputCaptured(rdr_own::Pass::Scene3D, b, frame);
}

// The test of a hook with the plants; a walk-time hook is asked at the clock g + 1 (it maps to decision g), so every hook of a frame answers one decision.
static bool QW(int i) {
  const bool walk = kWalkTime[i];
  if (walk) t_bias = 1;
  const bool r = Q(Hook(i));
  t_bias = 0;
  return r;
}

static int StampsFor(uint64_t frame) {
  int n = 0;
  for (const rdr_own::OwnedPassStamp& s : rdr_own::g_board.Take(frame))
    if (s.pass == uint8_t(rdr_own::Pass::Scene3D)) ++n;
  return n;
}

// Every hook, `rounds` times, in frame `frame`: true = every answer equal to `want`.
static bool AllHooks(bool want, int rounds = 3) {
  for (int r = 0; r < rounds; ++r)
    for (int i = 0; i < kHookCount; ++i)
      if (QW(i) != want) return false;   // S103 CLK2: walk-time hooks asked at g + 1 map to decision g (L2); section 11 drives the real cycle
  return true;
}

int main(int argc, char** argv) {
  g_plant = argc > 1 ? std::atoi(argv[1]) : 0;
  scene_frame::g_mode_cvar.store(&g_mode);
  g_clock.store(&FakeClock);

  // 1. mode 0 and 1 never skip, and never even touch the board
  ResetAll();
  Arm(3), rdr_own::SetReady(rdr_own::Pass::Scene3D, true);
  g_frame = 100, Mark(3, 100);
  g_mode = 0;
  Check(AllHooks(false), "mode0", "mode 0: every hook walks (never skips)");
  g_mode = 1;
  Check(AllHooks(false), "mode1", "mode 1 (mirror): every hook walks");
  Check(StampsFor(100) == 0, "mode01_board", "modes 0 / 1 write no stamp (the board is not touched)");

  // 2. mode 2, ready, inputs stamped: every hook skips; ONE stamp per frame for any number of hooks and calls
  g_mode = 2;
  Check(AllHooks(true), "ours", "mode 2, ready, inputs stamped for this frame: every hook skips");
  Check(StampsFor(100) == 1, "one_stamp", "one stamp for the frame after 16 hooks x 3 calls");
  const rdr_own::OwnedPassStamp st = rdr_own::g_board.Take(100)[0];
  Check(st.guest_frame == 100 && st.decision == uint8_t(rdr_own::Decision::Ours), "stamp_frame", "the stamp carries the hook clock (frame 100) and says Ours");

  // 3. readiness flips INSIDE the frame: the answer of frame 100 does not change for any hook; the next frame decides again
  rdr_own::SetReady(rdr_own::Pass::Scene3D, false);
  Check(AllHooks(true), "memo_ready_flip", "ready cleared mid-frame: every hook still answers the frame's first decision");
  g_frame = 101, Mark(3, 101);
  Check(AllHooks(false), "next_frame_walks", "frame 101 (not ready): every hook walks");
  Check(StampsFor(101) == 1, "one_stamp_walk", "one stamp for frame 101 (GameWalks)");
  rdr_own::SetReady(rdr_own::Pass::Scene3D, true);
  Check(AllHooks(false), "memo_walks", "ready set mid-frame 101: the frame stays the game's");

  // 4. a missing input: the frame walks; every input back: ours
  g_frame = 102, Mark(2, 102);   // bit 2 missing
  Check(AllHooks(false), "input_missing", "an input without this frame's stamp: every hook walks");
  g_frame = 103, Mark(3, 103);
  Check(AllHooks(true), "inputs_back", "all inputs stamped again: ours");
  g_frame = 104, Mark(3, 103);   // stamps of the previous frame (lag 0 input)
  Check(AllHooks(false), "stamp_old", "inputs stamped for frame 103 do not satisfy frame 104");

  // 5. a hold after a render-side miss: every hook walks for 60 frames, then ours again
  g_frame = 110, Mark(3, 110);
  Check(AllHooks(true), "pre_hold", "frame 110 ours");
  rdr_own::ReportMiss(rdr_own::Pass::Scene3D, 110);
  g_frame = 111, Mark(3, 111);
  Check(AllHooks(false), "hold", "after a miss: every hook walks (the hold)");
  g_frame = 110 + rdr_own::g_policy.hold_frames, Mark(3, 110 + rdr_own::g_policy.hold_frames);
  Check(AllHooks(true), "hold_ends", "the hold ends after hold_frames");

  // 6. two threads: the same answer for every hook, one stamp
  ResetAll();
  Arm(3), rdr_own::SetReady(rdr_own::Pass::Scene3D, true);
  g_mode = 2, g_frame = 200, Mark(3, 200);
  Check(Q(Hook::Model), "decide_main", "the first caller (main thread) decides frame 200: Ours");   // F4: decide once BEFORE the threads start, so the readiness flipper cannot race the first decision
  std::atomic<int> wrong{0};
  auto worker = [&] {
    for (int r = 0; r < 200; ++r)
      for (int i = 0; i < kHookCount; ++i)
        if (!QW(i)) ++wrong;
  };
  std::vector<std::thread> th;
  for (int t = 0; t < 4; ++t) th.emplace_back(worker);
  std::thread flipper([&] { for (int r = 0; r < 50; ++r) rdr_own::SetReady(rdr_own::Pass::Scene3D, (r & 1) != 0); });
  for (auto& t : th) t.join();
  flipper.join();
  Check(wrong == 0, "threads_same", "4 threads x 16 hooks x 200 calls (ready flipping): every answer equals the frame's first decision (Ours)");
  Check(StampsFor(200) == 1, "threads_one_stamp", "one stamp for frame 200 after the threaded run");

  // 7. a run of frames: the answer is decided once per frame (stamps == frames), counters add up
  ResetAll();
  Arm(1), rdr_own::SetReady(rdr_own::Pass::Scene3D, true);
  g_mode = 2;
  int ours_frames = 0;
  for (uint64_t f = 300; f < 340; ++f) {
    g_frame = f;
    if (f % 4 != 0) Mark(1, f);   // every 4th frame lacks its input
    const bool a = Q(Hook::Model);
    const bool all = AllHooks(a);
    if (!all) break;
    ours_frames += a ? 1 : 0;
    if (StampsFor(f) != 1) { ours_frames = -1000; break; }
  }
  Check(ours_frames == 30, "frame_run", "40 frames, every 4th without its input: 30 ours, 10 walks; every hook agrees; one stamp per frame");

  // 8. the hook table: names and count
  Check(kHookCount == 16 && std::string(HookName(Hook::Count)) == "?", "hooks", "16 hooks, names defined");
  Check(scene_frame::kHookToStream == 1 && scene_frame::HookFrameOf(101) == 100, "offset", "the stream frame is the hook frame + 1: scene_frame::HookFrameOf, applied by DrawOurs only");

  // 9. the mode snapshot (review F3): a cvar change after the snapshot does not change the mode until the next snapshot
  g_mode = 2;
  scene_frame::SnapshotMode();
  g_mode = 0;
  Check(scene_frame::CurrentMode() == scene_frame::Mode::Ours, "snapshot_holds", "cvar flipped to 0 after the snapshot: the frame still reads mode 2");
  scene_frame::SnapshotMode();
  Check(scene_frame::CurrentMode() == scene_frame::Mode::Off, "snapshot_next", "the next snapshot takes the new value");
  scene_frame::g_mode_snapshot.store(-1);
  g_mode = 2;
  Check(scene_frame::CurrentMode() == scene_frame::Mode::Ours, "snapshot_unset", "no snapshot taken (tests / rplay): the cvar is read directly");

  // 10. NotSkipped moves one call from skipped to walked (the crowd / particle early outs)
  ResetAll();
  g_counts.ours[int(Hook::Crowd)].store(5);
  NotSkipped(Hook::Crowd);
  Check(g_counts.ours[int(Hook::Crowd)] == 4 && g_counts.walks[int(Hook::Crowd)] == 1, "not_skipped", "an early-out call is counted walked");

  // 11. S103 CLK2: the real cycle, per-hook phase and per-bit input lag (FRAME_CONTRACT.md 3). One PERIOD j (the clock reads j after OnSwap(j)): the model-class hook asks on the UPDATE thread
  // (decision j, the first decider of j: it sees latch(j - 1)'s stamps, labelled j - 2), a walk-time hook (ring, plane) asks on the SWAP thread (decision j - 1, made in period j - 1), then
  // latch(j) stamps every latch input with label j - 1 (latch 13 stamps nothing: a hole). STREAM s holds the walks of the model-class hooks of period s - 1 and the ring of period s, and the render
  // side draws it from decision s - 1 (DrawOurs(s) = HookFrameOf(s)): the three must agree for every s, across the hole (disagreement = a hole (nothing draws it) or a double (both draw it)).
  ResetAll();
  const int kBits = 17;
  for (int b = 0; b < kBits; ++b) scene_frame::RequireInputLag(b, g_plant == 8 ? 1 : 2);   // plant 8: the lag CLK left in place for latch inputs
  scene_frame::RequireInputLag(kBits, 1);   // sphere_occ: stamped by the update thread with its own frame (lag 1)
  rdr_own::SetReady(rdr_own::Pass::Scene3D, true);
  g_mode = 2;
  scene_frame::g_mode_snapshot.store(-1);
  scene_frame::g_swap_thread_hash.store(scene_frame::ThisThreadHash());   // this thread = the swap thread; the model-class asks run on a spawned (update) thread
  uint64_t latched = 0;   // plant 7 only: the "latched record" clock of CLK's rule
  bool ring_ans[64] = {}, model_ans[64] = {}, dec_ours[64] = {};
  int agree_bad = 0, plane_bad = 0, ours_frames2 = 0;
  uint32_t first_missing_mask = 0;
  for (uint64_t j = 10; j < 30; ++j) {
    g_frame = j;                                           // OnSwap(j)
    std::thread update([&] {                               // the update thread's walk + update: the first decider of decision j
      if (g_plant == 7) {   // CLK's rule on a model-class hook: g - 1 before the latch
        const uint64_t f = latched < j ? j - 1 : j;
        model_ans[j] = scene_frame::CurrentMode() == scene_frame::Mode::Ours && scene_frame::Ours(f);
        if (model_ans[j]) scene_frame::NoteHook(f, int(Hook::Model), true);   // (what scene_skip::Ours does for a skipping hook)
      } else {
        model_ans[j] = Q(Hook::Model);
        if (Q(Hook::Crowd) != model_ans[j] || Q(Hook::ShadowModule) != model_ans[j]) ++agree_bad;
      }
      rdr_own::InputCaptured(rdr_own::Pass::Scene3D, kBits, j);   // sphere_occ: the update hook stamps its own frame (after the first decision of the period here)
    });
    update.join();
    if (g_plant == 11) {   // a wrong phase: the ring mapped to g like a model-class hook
      ring_ans[j] = scene_frame::Ours(j);
      if (ring_ans[j]) scene_frame::NoteHook(j, int(Hook::RingColour), true);
    } else {
      ring_ans[j] = Q(Hook::RingColour);                   // the swap thread: the walk-time hooks ask decision j - 1
    }
    if (Q(Hook::Plane) != ring_ans[j] || Q(Hook::RingColour2) != ring_ans[j]) ++plane_bad;
    if (j != 13) Mark(kBits, j - 1);                       // latch(j): the latch inputs stamped at hook j - 1
    latched = j;
    for (const rdr_own::OwnedPassStamp& x : rdr_own::g_board.Take(j))
      if (x.pass == uint8_t(rdr_own::Pass::Scene3D)) {
        dec_ours[j] = x.decision == uint8_t(rdr_own::Decision::Ours);
        if (!dec_ours[j] && j >= 11 && !first_missing_mask) first_missing_mask = ((1u << (kBits + 1)) - 1) & ~x.inputs_mask;
      }
  }
  int stream_bad = 0;
  for (uint64_t s2 = 12; s2 < 30; ++s2) {   // stream s: the ring of period s, the model-class walks of period s - 1 and the render side's decision s - 1 are one answer
    if (ring_ans[s2] != dec_ours[s2 - 1] || model_ans[s2 - 1] != dec_ours[s2 - 1]) ++stream_bad;
    if (dec_ours[s2 - 1]) ++ours_frames2;
    if (g_plant == 10 && s2 == 22) continue;               // never reaches NoteStream (NoRecord / dropped message with no census call)
    scene_frame::NoteStream(s2, dec_ours[s2 - 1] && !(g_plant == 9 && s2 == 20));   // plant 9: a render-side skip leaves the decided stream 20 undrawn   // the render side: DrawLive draws record s exactly when DrawOurs says decision s - 1 is OURS (run 38: 81 skipped decisions, 1 drawn frame)
  }
  Check(scene_frame::g_stream_skip_only == 0 && scene_frame::g_stream_draw_only == 0 && scene_frame::g_stream_both == uint64_t(ours_frames2) && scene_frame::g_stream_neither == uint64_t(18 - ours_frames2) && scene_frame::g_stream_partial == 0,
        "stream_census", "every stream the hooks skipped is drawn and every drawn stream was skipped (skip-only = hole 0, draw-only = double 0)");
  Check(scene_frame::CensusLine().find("streams skip+draw 17, skip-only 0, draw-only 0, neither 1, partial 0, evicted 0, never_rendered 0, ours_unpaired 1, render_skipped 0") != std::string::npos || g_plant != 0, "stream_census_line",
        "the [scene][frame] line carries the stream census; ours_unpaired = the one decision (29) whose stream is still in flight");
  Check(scene_frame::CensusLine().find("ours_unpaired 1") != std::string::npos, "unpaired_ours", "ours decisions minus (skip+draw + skip-only) is exactly the in-flight one: a decided stream no NoteStream reached shows as more (H2)");
  // per-hook census: the model hook skipped and drew for every OURS stream, the ring never in this cycle's census (it is not noted through scene_skip here)
  Check(scene_frame::g_hook_skip_only[int(Hook::Model)] == 0 && scene_frame::g_hook_walk_draw[int(Hook::Model)] == 0 && scene_frame::g_hook_skip_draw[int(Hook::Model)] == uint64_t(ours_frames2), "per_hook_census", "per hook: skip+draw for every OURS stream, no hole, no double");
  Check(stream_bad == 0 && agree_bad == 0 && plane_bad == 0, "stream_agree", "every stream s: ring (period s) == model-class (period s - 1) == decision s - 1 (what DrawOurs draws): no hole, no double across the 13 / 14 edge");
  Check(ours_frames2 == 17 && !dec_ours[14] && dec_ours[13] && dec_ours[15] && dec_ours[11], "cycle_decides", "decisions 11..29 are OURS except 14 (latch 13 stamped nothing). A lag-1 latch input misses every bit (run 37)");
  Check(g_counts.clock_swap[int(Hook::RingColour)] == 20 && g_counts.clock_other[int(Hook::RingColour)] == 0 && g_counts.clock_swap[int(Hook::Model)] == 0 && g_counts.clock_other[int(Hook::Model)] > 0,
        "census_by_thread", "the clock census splits asks by thread: the ring on the swap thread, the model hook on the update thread");
  Check(CensusLine().find("ring 20/0(walk)") != std::string::npos && CensusLine().find("model 0/") != std::string::npos, "census_line", "the census line names each hook's thread split and its phase");
  Check(kWalkTime[int(Hook::RingColour)] && kWalkTime[int(Hook::RingColour2)] && kWalkTime[int(Hook::Plane)] && !kWalkTime[int(Hook::Model)] && !kWalkTime[int(Hook::Crowd)], "phase_table", "ring, ring2, plane are walk-time; model and crowd are model-class");
  if (g_plant == 8 && first_missing_mask == ((1u << (kBits + 1)) - 1)) std::printf("      (run 37 signature seen: missing == required == 0x%x)\n", first_missing_mask);
  // S103 CLK2 (H2 / M2): the stream slot. partial = one decision with skipping AND walking hooks; evicted = a slot a newer frame took; never_rendered = a skipped slot replaced before any NoteStream read it.
  ResetAll();
  scene_frame::SnapshotMode();
  g_mode = 2;
  scene_frame::NoteHook(40, int(Hook::Model), true), scene_frame::NoteHook(40, int(Hook::Crowd), false);
  scene_frame::NoteStream(41, true);
  Check(scene_frame::g_stream_partial == 1 && scene_frame::g_stream_both == 1 && scene_frame::g_hook_walk_draw[int(Hook::Crowd)] == 1, "partial_counted", "a decision with a skipping and a walking hook is a partial stream (and the walker is a per-hook double)");
  scene_frame::NoteHook(50, int(Hook::Model), true);   // never read by a NoteStream ...
  scene_frame::NoteHook(50 + scene_frame::kSkipSlots, int(Hook::Model), true);   // ... then the slot is taken by a newer frame
  Check(scene_frame::g_stream_never_rendered == 1, "never_rendered", "a skipped slot replaced before the render side read it is counted, not lost");
  scene_frame::NoteStream(51, false);   // the render side reads frame 50's slot: a newer frame holds it
  Check(scene_frame::g_stream_evicted == 1 && scene_frame::g_stream_skip_only == 0 && scene_frame::g_stream_neither == 0, "evicted", "a stream read from a slot a newer frame took is counted evicted, never as a verdict");
  scene_frame::NoteHook(70, int(Hook::Model), true), scene_frame::NoteStream(71, true), scene_frame::NoteHook(70 + scene_frame::kSkipSlots, int(Hook::Model), true);
  Check(scene_frame::g_stream_never_rendered == 1, "paired_not_lost", "a paired slot replaced later is not a never-rendered stream");
  scene_frame::NoteHook(80, int(Hook::Model), true), scene_frame::NoteHook(80, int(Hook::Model), true), scene_frame::UnnoteSkip(80, int(Hook::Model));
  scene_frame::NoteStream(81, true);
  scene_frame::NoteHook(90, int(Hook::Model), true);
  scene_frame::NoteStream(91, false, true);
  Check(scene_frame::g_stream_render_skipped == 1 && scene_frame::g_stream_skip_only == 0, "render_skip_bucket", "a dropped render frame counts apart (render_skipped): no hole, no double; the unpaired-ours sum includes it");
  Check(scene_frame::g_stream_draw_only == 1, "unnote_early_out", "an early-out (NotSkipped) takes the hook's skip bit back: the drawn stream is draw-only, not skip+draw");
  {   // M2: four threads set their own bit of the same slot for 3000 frames, each frame replacing the slot: no bit is ever lost (one CAS word)
    ResetAll();
    std::atomic<int> arrived{0};
    std::atomic<uint64_t> lost{0};
    auto run = [&](int me) {
      for (uint64_t f = 100; f < 3100; ++f) {
        scene_frame::NoteHook(f, me, true);
        arrived.fetch_add(1);
        while (arrived.load() < int((f - 99) * 4)) {}   // lockstep: every thread has noted frame f
        if (me == 0) {
          const uint64_t w = scene_frame::g_skip_slots[f % scene_frame::kSkipSlots].load();
          if ((w & scene_frame::kSlotFrameMask) != (f & scene_frame::kSlotFrameMask) || (w >> 32 & 0xFFFFu) != 0xFu) lost.fetch_add(1);
        }
        arrived.fetch_add(0);
      }
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back(run, t);
    for (auto& t : ts) t.join();
    Check(lost == 0, "slot_atomic", "4 threads x 3000 frames noting one slot each frame: every frame's slot holds all 4 bits (no lost reset)");
  }
  // the lag windows: lag 2 takes f - 2 .. f and not f - 3; lag 1 (the sphere) takes f - 1 and f, not f - 2; the frame 0 / 1 clamp never wraps
  ResetAll();
  scene_frame::RequireInputLag(0, 2), scene_frame::RequireInputLag(1, 1);
  rdr_own::InputCaptured(rdr_own::Pass::Scene3D, 0, 98), rdr_own::InputCaptured(rdr_own::Pass::Scene3D, 1, 99);
  Check(scene_frame::InputsMask(100) == 3u, "lag_window_ok", "f = 100: lag-2 stamp 98 and lag-1 stamp 99 both count");
  rdr_own::InputCaptured(rdr_own::Pass::Scene3D, 0, 97), rdr_own::InputCaptured(rdr_own::Pass::Scene3D, 1, 98);
  Check(scene_frame::InputsMask(100) == 0u, "lag_window_old", "f = 100: lag-2 stamp 97 and lag-1 stamp 98 are too old");
  rdr_own::InputCaptured(rdr_own::Pass::Scene3D, 1, 100);
  Check((scene_frame::InputsMask(100) & 2u) == 2u, "lag_window_same", "f = 100: a lag-1 input the update hook already stamped this period (frame 100) counts");
  Check(scene_frame::InputsMask(0) == 0u && scene_frame::InputsMask(1) == 0u, "lag_clamp", "frames 0 / 1: no unsigned wrap lets an old stamp pass");

  std::printf("%s\n", g_fail ? "FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}
