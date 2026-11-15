// S102 H3: --wwe2k14_frame_split (default off): the per-frame split of the game's render-command (swap) thread, as seven csv columns at the END of frames_NNN.csv.
// The arithmetic and the column definitions are src\frame_split_math.h (read its header first). This file gathers the counters on the swap thread:
//   * the reader's draw / present time: the reader already times both for its census (reader.h g_times, rdtsc ticks, one pair per draw TODAY); a function the
//     reader registers hands the two totals over as nanoseconds. NO new clock on the per-draw path, on or off.
//   * the scene-live EndFrame, the replay walk, the fence waits and the frame-barrier wait: a steady_clock pair each, per frame / per wait (not per draw or per
//     record), taken only while g_on is set (one relaxed load otherwise). The sums are thread_local: OnSwap runs on the swap thread, so another thread's waits
//     (the update thread's own fence waits) never reach the row.
// Instruments never change the game's device or hot path: nothing here touches a guest register or memory.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "frame_split_math.h"

namespace wwe2k14::frame_split {

inline std::atomic<bool> g_on{false};  // set ONCE, when the csv opens (FrameStats::OpenCsv, from the cvar: F6); read relaxed by the hooks

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct ReaderNs {
  uint64_t draw = 0, present = 0;  // cumulative ns, the reader's g_times.draw / g_times.present
};
inline ReaderNs (*g_reader_read)() = nullptr;  // registered by reader.h (a plain pointer: frame_stats.h does not include the reader)

struct ThreadSums {
  Counters c;
  uint64_t walk_start = 0, fence_start = 0, barrier_start = 0;
  ReaderNs walk_reader;  // the reader's totals at the walk's start / last cut (F1: what ran inside the walk = the difference)
  bool in_walk = false;
};
inline thread_local ThreadSums t_sums;

// sub_82BB5BE8's hook (replay_watch.h): once per frame.
inline void WalkAddReader(ThreadSums& s) {  // F1: the reader's draw / present time that ran since the walk's start / last cut
  if (!g_reader_read) return;
  const ReaderNs r = g_reader_read();
  s.c.reader_draw_in_walk += r.draw >= s.walk_reader.draw ? r.draw - s.walk_reader.draw : 0;
  s.c.reader_present_in_walk += r.present >= s.walk_reader.present ? r.present - s.walk_reader.present : 0;
  s.walk_reader = r;
}
inline void WalkBegin() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  t_sums.in_walk = true;
  if (g_reader_read) t_sums.walk_reader = g_reader_read();
  t_sums.walk_start = NowNs();
}
inline void WalkEnd() {
  ThreadSums& s = t_sums;
  if (!s.in_walk) return;  // (opened while off, or already ended)
  s.c.walk += NowNs() - s.walk_start;
  WalkAddReader(s);
  s.in_walk = false;
}
inline void WalkCut() {  // at a frame boundary inside the walk: the walk so far belongs to this row
  ThreadSums& s = t_sums;
  if (!s.in_walk) return;
  const uint64_t now = NowNs();
  s.c.walk += now - s.walk_start;
  s.walk_start = now;
  WalkAddReader(s);
}

// The walk of one hook call, whichever way it leaves (W3's native loop returns early; the recompiled walk falls through).
struct WalkScope {
  WalkScope() { WalkBegin(); }
  ~WalkScope() { WalkEnd(); }
  WalkScope(const WalkScope&) = delete;
  WalkScope& operator=(const WalkScope&) = delete;
};

// gpu_timeline.h's gwait_b / gwait_e (a wait: per frame a handful).
inline void FenceBegin() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  t_sums.fence_start = NowNs();
}
inline void FenceEnd() {
  ThreadSums& s = t_sums;
  if (!s.fence_start) return;
  const uint64_t d = NowNs() - s.fence_start;
  s.fence_start = 0;
  s.c.fence += d;
  if (s.in_walk) s.c.fence_in_walk += d;
}

// barrier_probe.h's wait_begin / wait_end (sub_82C3BAE0: once per frame).
inline void BarrierBegin() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  t_sums.barrier_start = NowNs();
}
inline void BarrierEnd() {
  ThreadSums& s = t_sums;
  if (!s.barrier_start) return;
  s.c.barrier += NowNs() - s.barrier_start;
  s.barrier_start = 0;
}

// reader.h: around rdr_scene::live::EndFrame (in_walk is measured: it is only added to the walk's parts when the walk is open).
struct ScopedSceneEnd {
  uint64_t t0 = 0;
  ScopedSceneEnd() {
    if (g_on.load(std::memory_order_relaxed)) t0 = NowNs();
  }
  ~ScopedSceneEnd() {
    if (!t0) return;
    const uint64_t d = NowNs() - t0;
    t_sums.c.scene_end += d;
    if (t_sums.in_walk) t_sums.c.scene_end_in_walk += d;  // F1: EndFrame normally runs AFTER the walk (sub_82BB5CC0), outside it
  }
};

// FrameStats::OnSwap: this thread's cumulative counters now (the walk cut first).
inline Counters Gather() {
  WalkCut();
  Counters c = t_sums.c;
  if (g_reader_read) {
    const ReaderNs r = g_reader_read();
    c.reader_draw = r.draw;
    c.reader_present = r.present;
  }
  return c;
}

}  // namespace wwe2k14::frame_split
