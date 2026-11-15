// S102 H3: the per-frame split of the game's render-command (swap) thread: the pure arithmetic, no game types, no clocks (tools\frame_split\frame_split_test.cpp
// feeds it synthetic counters; src\frame_split.h gathers the real ones; src\frame_stats.h writes the columns, behind --wwe2k14_frame_split, default off).
//
// The thread is one thread: the game's render-command loop (CRenderCmdManager, sub_82BB5CC0) replays the last frame's command list (sub_82BB5BE8), and a
// VdSwap record inside that list calls FrameStats::OnSwap (one csv row). Every counter below is a CUMULATIVE nanosecond total for that thread; a row is the
// difference of two snapshots (Splitter::Take), so a part's own timer is never reset and the row cannot double-count across frames.
//
// Nesting (S102 H3, MEASURED, not assumed; the order is the loop of sub_82BB5CC0, read from the disassembly, F1 of the review):
//   per iteration of sub_82BB5CC0:  walk `bl 0x82BB5BE8` (0x82BB5F34)  ->  barrier wait (0x82BB5F5C / 60)  ->  present `bl 0x82BA84E8` (0x82BB5FF4)  ->
//   0x82C227E8  ->  sub_82CB37C0 (the reader's OnPresent hook: the live hand-over, rdr_scene::live::EndFrame). So the reader's PRESENT and the scene EndFrame run
//   AFTER the walk, outside it; the reader's DRAW calls run inside it (the replayed commands). OnSwap (one csv row) runs on this same thread (F8: confirmed by the
//   disassembly; it is called from the swap record, i.e. inside the walk, and a walk that spans a frame boundary is cut there and shared between the two rows).
//   Because the nesting can differ between paths (W3's native loop, the recompiled one, a swap record that presents inline), it is MEASURED: WalkBegin / WalkEnd /
//   WalkCut snapshot the reader's draw / present totals and every ScopedSceneEnd asks "am I inside the walk?", so the in-walk parts are counted apart:
//   walk            sub_82BB5BE8's whole walk (hook in replay_watch.h; a WalkScope), cut at every OnSwap.
//   reader_draw     the reader's draw timers (g_times.draw): the whole total is the column; reader_draw_in_walk = the part inside the walk (a running total, F2:
//                   g_times is reset every 600 frames by the reader's census, RunningTicks banks it first).
//   reader_present  the reader's present timer (g_times.present): column = total; reader_present_in_walk = the part that ran inside the walk (usually 0).
//   scene_end       rdr_scene::live::EndFrame; column = total; scene_end_in_walk = the part inside the walk (usually 0).
//   fence_wait      the game's GPU waits (gwait_b .. gwait_e hooks, gpu_timeline.h): WALL time; fence_in_walk = the part inside the walk.
//                   UNCONFIRMED: a fence wait nested inside a reader draw (SyncGpuFetch) would be taken off the replay twice; "replay" would then read low.
//   barrier_wait    the wait for the update thread's frame barrier (sub_82C3BAE0, barrier_probe.h hooks): WALL time, outside the walk.
// Columns (ms per guest frame on this thread, appended after present_call_ms in this order):
//   fs_reader_draw_ms, fs_reader_present_ms, fs_scene_end_ms, fs_replay_ms, fs_fence_wait_ms, fs_barrier_wait_ms, fs_rest_ms
//   fs_replay_ms = walk - reader_draw_in_walk - reader_present_in_walk - scene_end_in_walk - fence_in_walk
//                  = the game's interpreter + the loop + our untimed hook work (F8: NOT "the interpreter alone": the hooks' own work that has no timer is in it)
//   fs_rest_ms   = thread CPU (frame_stats' game_cpu_ms) - reader_draw - reader_present - scene_end - replay. CPU minus wall-time parts (F3): the fence wait SPINS (it is
//                  CPU), so it sits in rest, not out of it; the barrier wait (a kernel wait) is not CPU. Rest CAN BE NEGATIVE: a reader part that waits inside itself
//                  (the worker join in present) is wall time above its CPU. Empty cell when the thread CPU is unknown (F4), never -1.
//   present_gap_ms of row N may belong to an earlier guest frame when frames are queued (F8).
#pragma once

#include <cmath>
#include <cstdint>

namespace wwe2k14::frame_split {

// Names, in csv order. The header and the python parsers read this list (tools\frame_split\frame_split_check.py compares them).
inline constexpr const char* kColumnNames[] = {"fs_reader_draw_ms", "fs_reader_present_ms", "fs_scene_end_ms", "fs_replay_ms",
                                               "fs_fence_wait_ms",  "fs_barrier_wait_ms",   "fs_rest_ms"};
inline constexpr int kColumns = 7;

struct Counters {  // cumulative ns on the swap thread
  uint64_t reader_draw = 0, reader_present = 0, scene_end = 0, walk = 0, fence = 0, fence_in_walk = 0, barrier = 0;
  uint64_t reader_draw_in_walk = 0, reader_present_in_walk = 0, scene_end_in_walk = 0;  // F1: the parts measured INSIDE the walk
};

// F2: a total that survives its owner's reset. The reader zeroes g_times every 600 frames (CensusMaybe, inside OnPresent, BEFORE OnSwap); Bank() adds the values
// into the running total first, Total() = banked + current. Plain ticks (the caller converts).
struct RunningTicks {
  uint64_t draw = 0, present = 0;
  void Bank(uint64_t cur_draw, uint64_t cur_present) { draw += cur_draw, present += cur_present; }
  uint64_t Draw(uint64_t cur) const { return draw + cur; }
  uint64_t Present(uint64_t cur) const { return present + cur; }
};

struct Row {
  bool valid = false;  // false on the first snapshot (nothing to subtract from)
  double ms[kColumns] = {};  // ms[6] (rest) is NaN when the thread CPU was unknown: the writer prints an empty cell (F4)
};

class Splitter {
 public:
  uint64_t backwards() const { return backwards_; }  // how many counters went backwards so far (frame_stats logs the first one)
  // `now`: the counters at this guest frame; `thread_cpu_ms`: this thread's CPU time since the last row (< 0 = unknown: fs_rest_ms is then NaN = an empty cell).
  Row Take(const Counters& now, double thread_cpu_ms) {
    Row row;
    if (primed_) {
      const Counters d = Diff(now, last_);
      row.valid = true;
      const double draw = ToMs(d.reader_draw), present = ToMs(d.reader_present), scene_end = ToMs(d.scene_end);
      const double replay = ToMs(d.walk) - ToMs(d.reader_draw_in_walk) - ToMs(d.reader_present_in_walk) - ToMs(d.scene_end_in_walk) - ToMs(d.fence_in_walk);
      row.ms[0] = draw;
      row.ms[1] = present;
      row.ms[2] = scene_end;
      row.ms[3] = replay;
      row.ms[4] = ToMs(d.fence);
      row.ms[5] = ToMs(d.barrier);
      row.ms[6] = thread_cpu_ms >= 0 ? thread_cpu_ms - draw - present - scene_end - replay : std::nan("");
    }
    last_ = now;
    primed_ = true;
    return row;
  }

 private:
  static double ToMs(uint64_t ns) { return double(ns) / 1e6; }
  uint64_t Sub(uint64_t a, uint64_t b) {  // (a counter that went backwards = a restart: count nothing, and say so once through backwards())
    if (a >= b) return a - b;
    ++backwards_;
    return 0;
  }
  Counters Diff(const Counters& a, const Counters& b) {
    return {Sub(a.reader_draw, b.reader_draw), Sub(a.reader_present, b.reader_present), Sub(a.scene_end, b.scene_end), Sub(a.walk, b.walk),
            Sub(a.fence, b.fence), Sub(a.fence_in_walk, b.fence_in_walk), Sub(a.barrier, b.barrier),
            Sub(a.reader_draw_in_walk, b.reader_draw_in_walk), Sub(a.reader_present_in_walk, b.reader_present_in_walk), Sub(a.scene_end_in_walk, b.scene_end_in_walk)};
  }
  Counters last_;
  bool primed_ = false;
  uint64_t backwards_ = 0;
};

}  // namespace wwe2k14::frame_split
