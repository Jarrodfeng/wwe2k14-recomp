// S102 H3: offline test of the swap-thread split (src/frame_split_math.h + src/frame_split.h). 0 game runs.
//   clang++ -std=c++20 -O2 -I <dir with frame_split_math.h first> -I src tools/frame_split/frame_split_test.cpp
// Prints PASS / FAIL lines, "cost" lines and "all checks passed" (exit 0) or the failing count (exit 1). frame_split_check.py builds it against the shipped headers
// and against planted copies (a part counted twice, a snapshot not reset, a fence counted twice), each of which must exit 1.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "frame_split.h"

using namespace wwe2k14::frame_split;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
static bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

struct Frame {  // the truth for one synthetic frame, ms
  double draw, present, scene_end, replay_self, fence_in_walk, fence_out_walk, barrier, cpu_extra;
};

static std::vector<Frame> MakeFrames(int n) {
  std::vector<Frame> v;
  for (int i = 0; i < n; ++i) {
    Frame f;
    f.draw = 7.5 + 0.1 * (i % 7);
    f.present = 1.25 + 0.05 * (i % 5);
    f.scene_end = 3.5 + 0.2 * (i % 3);
    f.replay_self = 4.0 + 0.3 * (i % 11);
    f.fence_in_walk = (i % 4 == 0) ? 0.0 : 0.5 * (i % 4);
    f.fence_out_walk = (i % 6 == 0) ? 0.25 : 0.0;
    f.barrier = 0.75 + 0.01 * (i % 13);
    f.cpu_extra = 0.4 + 0.02 * (i % 9);  // the "rest": thread CPU the parts do not hold
    v.push_back(f);
  }
  return v;
}
static uint64_t Ns(double ms) { return uint64_t(std::llround(ms * 1e6)); }

static void TestArithmetic() {
  const auto frames = MakeFrames(60);
  Splitter sp;
  Counters cum;
  bool all_ok = true, first_invalid = false;
  double sum_rest_expected = 0, sum_rest_got = 0;
  for (size_t i = 0; i < frames.size(); ++i) {
    const Frame& f = frames[i];
    // F1: the present and the scene EndFrame run AFTER the walk (sub_82BB5CC0), outside it; only the draw is inside.
    cum.reader_draw += Ns(f.draw);
    cum.reader_draw_in_walk += Ns(f.draw);
    cum.reader_present += Ns(f.present);
    cum.scene_end += Ns(f.scene_end);
    cum.walk += Ns(f.draw + f.replay_self + f.fence_in_walk);
    cum.fence += Ns(f.fence_in_walk + f.fence_out_walk);
    cum.fence_in_walk += Ns(f.fence_in_walk);
    cum.barrier += Ns(f.barrier);
    const double cpu = f.draw + f.present + f.scene_end + f.replay_self + f.cpu_extra;  // (a fence wait is wall time, not part of this CPU figure)
    const Row r = sp.Take(cum, cpu);
    if (i == 0) {
      first_invalid = !r.valid;
      continue;
    }
    if (!r.valid) all_ok = false;
    // Every column equals this frame's own increment, never a running sum.
    all_ok = all_ok && Near(r.ms[0], f.draw) && Near(r.ms[1], f.present) && Near(r.ms[2], f.scene_end) && Near(r.ms[3], f.replay_self) &&
             Near(r.ms[4], f.fence_in_walk + f.fence_out_walk) && Near(r.ms[5], f.barrier) && Near(r.ms[6], f.cpu_extra);
    sum_rest_expected += f.cpu_extra, sum_rest_got += r.ms[6];
  }
  Check(first_invalid, "the first snapshot has no row (nothing to subtract from)");
  Check(all_ok, "59 synthetic frames: every column equals that frame's own increment (draw, present, scene end, replay = walk minus our parts and the fence, fence, barrier, rest)");
  Check(Near(sum_rest_expected, sum_rest_got), "the rest column sums to the CPU the parts do not hold (no part counted twice)");
}

static void TestIdentity() {
  // One frame with every part present: thread CPU = draw + present + scene_end + replay + rest, whatever the fence / barrier are (they are waits).
  Splitter sp;
  Counters a;
  sp.Take(a, 0);
  Counters b;
  b.reader_draw = Ns(10), b.reader_present = Ns(2), b.scene_end = Ns(3), b.walk = Ns(10 + 2 + 3 + 4 + 5), b.fence = Ns(5), b.fence_in_walk = Ns(5), b.barrier = Ns(6);
  b.reader_draw_in_walk = Ns(10), b.reader_present_in_walk = Ns(2), b.scene_end_in_walk = Ns(3);  // (here the present and the end ran inside the walk: measured, so taken off)
  const Row r = sp.Take(b, 20.0);
  Check(r.valid && Near(r.ms[3], 4.0), "replay = walk 24 - draw 10 - present 2 - scene end 3 - fence in walk 5 = 4");
  Check(Near(r.ms[6], 20.0 - 10 - 2 - 3 - 4), "rest = thread CPU 20 - draw - present - scene end - replay = 1");
  Check(Near(r.ms[4], 5.0) && Near(r.ms[5], 6.0), "fence 5 and barrier 6 are reported as measured, not subtracted from the CPU rest");
  Splitter sq;
  sq.Take(a, 0);
  Check(std::isnan(sq.Take(b, -1.0).ms[6]), "an unknown thread CPU gives rest = NaN (the writer prints an empty cell), never -1 or a made-up number (F4)");
  Splitter sr;
  sr.Take(b, 0);
  Counters back = b;
  back.reader_draw = Ns(1);
  Check(Near(sr.Take(back, 0).ms[0], 0.0), "a counter that went backwards (a restart) counts 0, not 2^64");
  Check(sr.backwards() >= 1 && sp.backwards() == 0, "a counter that went backwards is counted (frame_stats logs the first), a clean run counts none");
}

// F2: the reader's g_times is zeroed every 600 frames INSIDE OnPresent, before OnSwap reads it. RunningTicks banks it first.
static uint64_t g_cur_draw = 0, g_cur_present = 0;  // the "reader's" resettable counters
static RunningTicks g_run;
static ReaderNs FakeResettingReader() { return {g_run.Draw(g_cur_draw), g_run.Present(g_cur_present)}; }
static void TestRunningTicks() {
  g_reader_read = &FakeResettingReader;
  g_on.store(true);
  t_sums = ThreadSums{};
  Splitter sp;
  g_cur_draw = g_cur_present = 0;
  g_run = RunningTicks{};
  bool ok = true;
  for (int frame = 0; frame < 1500; ++frame) {
    g_cur_draw += Ns(8.0), g_cur_present += Ns(1.0);
    if (frame % 600 == 599) {  // the census: bank, then reset, all before the row
      g_run.Bank(g_cur_draw, g_cur_present);
      g_cur_draw = g_cur_present = 0;
    }
    const Row r = sp.Take(Gather(), 20.0);
    if (frame > 0) ok = ok && Near(r.ms[0], 8.0) && Near(r.ms[1], 1.0);
  }
  Check(ok && sp.backwards() == 0, "a reader that resets its counters mid-run (every 600 frames) keeps fs_reader_draw_ms / fs_reader_present_ms continuous (F2)");
  g_reader_read = nullptr;
  g_on.store(false);
}

static void TestNames() {
  const char* want[] = {"fs_reader_draw_ms", "fs_reader_present_ms", "fs_scene_end_ms", "fs_replay_ms", "fs_fence_wait_ms", "fs_barrier_wait_ms", "fs_rest_ms"};
  bool ok = kColumns == 7;
  for (int i = 0; ok && i < 7; ++i) ok = std::strcmp(kColumnNames[i], want[i]) == 0;
  Check(ok, "seven columns, in the documented order");
}

static void Spin(int ms) {  // a busy wait: sleep_for rounds to the 15.6 ms timer tick on Windows
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
  }
}

// F1: the real gatherers driven in the order of sub_82BB5CC0's loop (walk -> barrier -> present -> EndFrame -> the row), with a fake reader and known spans.
static ReaderNs g_fake;
static ReaderNs FakeReader() { return g_fake; }
static double SpinMs(double ms) {
  const uint64_t t0 = NowNs();
  while (NowNs() - t0 < uint64_t(ms * 1e6)) {
  }
  return double(NowNs() - t0) / 1e6;
}
static void TestSequence() {
  g_reader_read = &FakeReader;
  g_on.store(true);
  t_sums = ThreadSums{};
  g_fake = ReaderNs{};
  Splitter sp;
  sp.Take(Gather(), -1.0);
  bool ok_replay = true, ok_parts = true, ok_inline = true;
  double worst = 0;
  for (int variant = 0; variant < 7; ++variant) {
    const bool inline_present = variant >= 4;  // a swap record that presents inside the walk: measured, still not the interpreter's
    double interp = 0;
    WalkBegin();
    interp += SpinMs(2.0);
    g_fake.draw += uint64_t(SpinMs(1.5) * 1e6);  // a reader draw inside the walk (the fake totals follow the real span)
    FenceBegin();
    SpinMs(1.0);
    FenceEnd();
    interp += SpinMs(1.0);
    if (inline_present) {
      g_fake.present += uint64_t(SpinMs(1.25) * 1e6);
      ScopedSceneEnd e;
      SpinMs(0.75);
    }
    WalkEnd();
    BarrierBegin();
    SpinMs(0.5);
    BarrierEnd();
    if (!inline_present) {  // sub_82BB5CC0 after the barrier: present, then the reader's OnPresent (EndFrame)
      g_fake.present += uint64_t(SpinMs(1.25) * 1e6);
      ScopedSceneEnd e;
      SpinMs(0.75);
    }
    const Row r = sp.Take(Gather(), -1.0);
    if (variant == 0) continue;
    worst = std::fmax(worst, std::fabs(r.ms[3] - interp));
    ok_replay = ok_replay && std::fabs(r.ms[3] - interp) < 0.8;
    ok_parts = ok_parts && r.ms[0] >= 1.4 && r.ms[0] < 3.0 && r.ms[1] >= 1.2 && r.ms[2] >= 0.7 && r.ms[4] >= 0.9 && r.ms[5] >= 0.4;
    ok_inline = ok_inline && r.ms[3] >= 0.0;
  }
  std::printf("sequence: worst |replay - injected interpreter| = %.3f ms\n", worst);
  Check(ok_replay, "replay equals the injected interpreter time when the present and EndFrame run AFTER the walk, and when they run inside it (F1, sub_82BB5CC0 order)");
  Check(ok_parts && ok_inline, "draw, present, scene end, fence and barrier read their own spans in the same sequence; replay never negative");
  g_reader_read = nullptr;
  g_on.store(false);
  t_sums = ThreadSums{};
}

static void TestHooks() {
  // The real gatherers on this thread: the walk, a fence wait inside it, a barrier wait outside, with sleeps of known length.
  g_on.store(true);
  t_sums = ThreadSums{};
  Splitter sp;
  sp.Take(Gather(), 0);
  WalkBegin();
  Spin(3);
  FenceBegin();
  Spin(2);
  FenceEnd();
  const Row r = sp.Take(Gather(), -1.0);  // cut inside the walk
  Spin(2);
  WalkEnd();
  BarrierBegin();
  Spin(1);
  BarrierEnd();
  const Row r2 = sp.Take(Gather(), -1.0);
  Check(r.valid && r.ms[4] >= 1.9 && r.ms[4] < 8.0, "a 2 ms fence wait reads about 2 ms");
  Check(r.ms[3] >= 2.9 && r.ms[3] < 9.0, "the replay of a 3 ms + 2 ms fence walk cut mid-walk reads about 3 ms (the fence is taken off)");
  Check(r2.ms[3] >= 1.9 && r2.ms[3] < 8.0 && r2.ms[4] < 0.5, "the walk's second part (2 ms) lands in the next row; the fence is not counted again");
  Check(r2.ms[5] >= 0.9 && r2.ms[5] < 7.0, "a 1 ms barrier wait outside the walk reads about 1 ms");
  std::thread other([] {  // another thread's waits never reach this thread's sums
    FenceBegin();
    Spin(2);
    FenceEnd();
  });
  other.join();
  const Row r3 = sp.Take(Gather(), -1.0);
  Check(r3.ms[4] < 0.01, "another thread's fence wait is not in this thread's row (thread_local sums)");
  g_on.store(false);
  t_sums = ThreadSums{};
  FenceBegin();
  Spin(1);
  FenceEnd();
  WalkBegin();
  WalkEnd();
  Check(t_sums.c.fence == 0 && t_sums.c.walk == 0, "with the switch off the hooks count nothing");
}

static void Cost() {
  // The same operations a frame does when the switch is on: 2 clock pairs (walk, scene end) + the walk cut + ~2 fence pairs + 1 barrier pair, Gather, Take, and
  // the seven-cell snprintf. 'off' = the six relaxed loads the hooks do when it is off.
  constexpr int kN = 200000;
  g_on.store(true);
  t_sums = ThreadSums{};
  Splitter sp;
  char text[128];
  uint64_t sink = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    WalkBegin();
    {
      ScopedSceneEnd s;
    }
    FenceBegin();
    FenceEnd();
    FenceBegin();
    FenceEnd();
    BarrierBegin();
    BarrierEnd();
    const Row r = sp.Take(Gather(), 14.0);
    size_t at = 0;
    for (int c = 0; c < kColumns; ++c) at += size_t(std::snprintf(text + at, sizeof(text) - at, ",%.3f", r.ms[c]));
    sink += at;
    WalkEnd();
  }
  const double on_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kN;
  g_on.store(false);
  const auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    WalkBegin();
    {
      ScopedSceneEnd s;
    }
    FenceBegin();
    FenceEnd();
    FenceBegin();
    FenceEnd();
    BarrierBegin();
    BarrierEnd();
    WalkEnd();
  }
  const double off_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t1).count() / kN;
  std::printf("cost on: %.0f ns per frame (13 clock reads + Take + 7 cells; this PC, steady_clock; sink %llu)\n", on_ns, (unsigned long long)sink);
  std::printf("cost off: %.1f ns per frame (the hooks' relaxed loads)\n", off_ns);
  Check(on_ns < 20000.0, "the instrument costs under 20 us a frame when on");
}

int main() {
  TestArithmetic();
  TestIdentity();
  TestNames();
  TestRunningTicks();
  TestSequence();
  TestHooks();
  Cost();
  if (g_fail) {
    std::printf("%d check(s) FAILED\n", g_fail);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
