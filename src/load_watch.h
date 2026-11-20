// wwe2k14 - loader watch (S105 A6W; REPORT_025: the phone's Move-set load hung 46 s while the editor drew at 60 fps and the S103 STALL
// note never fired). Pure logic + counters, no SDK includes (unit test tools\tests\load_watch_test.cpp).
//
// Why the S103 StallWatch (load_stall.h) missed it: it is fed ONLY by the loader worker's own step, so a worker blocked in a wait never
// feeds it; it restarts after any > 1 s gap and fires only in state 9. State 9 is the HAND-OFF (docs/research/s105/A6.md): the worker
// waits for the read-end thread (FileReadEndProcessThread, object [0x8394024C]) and then sets [0x839400A4] = 0, so a stuck completion
// can leave NO current request at all.
// This watch is fed from its OWN host thread (load_probe.h StartLoadWatch, every 250 ms). "Something is loading" = a current request
// [0x839400A4], OR a read-end job in flight [[0x8394024C]+40], OR a non-empty queue [0x839400A8]. Progress = the (request, state, job,
// queue) words changing, a PAC file opened, a PAC file read returning, a read-end completion finishing. It fires ONCE when something is
// loading and nothing moved for limit_us while >= min_frames guest frames advanced. The loader pause flag [0x8362A0B0] != 0 (the step is
// skipped on purpose, a stream / movie change) never fires. Worker steps alone are NOT progress (a worker that steps while the request
// never advances is exactly the "state 6 never completes" picture). LOG ONLY.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wwe2k14::load_probe {

// Progress counters, relaxed atomics (a lost increment would only delay a fire by one tick, never cause one).
inline std::atomic<uint64_t> g_loader_opens{0};        // kernel_overrides.h LogFileOpen: PAC files opened (path ends .pac)
inline std::atomic<uint64_t> g_loader_reads{0};        // file_trace.h: reads returning on a handle that opened a PAC file
inline std::atomic<uint64_t> g_loader_completions{0};  // load_probe.h sub_82C37C08 (read-end callback) returned
inline std::atomic<int64_t> g_worker_last_step_us{0};  // load_probe.h sub_82C37070 hook: Us() after each worker step (0 = never)
inline std::atomic<bool> g_load_watch_stop{false};     // set first thing in OnShutdown: the watch thread must not touch the guest after it

// PAC path filter: ends in ".pac", any case. (Other threads read other files all the time: Wwise streams, Bink, saves.)
inline bool IsPacPath(const char* p, size_t n) {
  if (n < 4) return false;
  const char* e = p + n - 4;
  auto lo = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
  return e[0] == '.' && lo(e[1]) == 'p' && lo(e[2]) == 'a' && lo(e[3]) == 'c';
}

// The handles that opened a PAC file, lock-free: a small ring of atomic slots. A stale slot after the handle number is reused only
// over-counts reads (delays a fire, never causes one). Handle 0 / 0xFFFFFFFF are never stored.
struct PacHandles {
  static constexpr uint32_t kSlots = 64;
  std::atomic<uint32_t> slot[kSlots];
  std::atomic<uint32_t> next{0};
  PacHandles() { for (auto& s : slot) s.store(0, std::memory_order_relaxed); }
  void Add(uint32_t h) {
    if (h == 0 || h == 0xFFFFFFFFu) return;
    slot[next.fetch_add(1, std::memory_order_relaxed) % kSlots].store(h, std::memory_order_relaxed);
  }
  bool Has(uint32_t h) const {
    if (h == 0) return false;
    for (const auto& s : slot) if (s.load(std::memory_order_relaxed) == h) return true;
    return false;
  }
};
inline PacHandles g_pac_handles;

// The hook sites call these (pure of the SDK).
// The decisions are pure (unit-tested); NoteOpen / NoteRead are the two-line glue the hooks call (a static check in the unit asserts the sites).
inline bool OpenCounts(const char* path, size_t n, uint32_t result) { return result == 0 && IsPacPath(path, n); }
inline void NoteOpen(const char* path, size_t n, uint32_t result, uint32_t handle) {
  if (!OpenCounts(path, n, result)) return;
  g_pac_handles.Add(handle);
  g_loader_opens.fetch_add(1, std::memory_order_relaxed);
}
inline void NoteRead(uint32_t handle) {
  if (g_pac_handles.Has(handle)) g_loader_reads.fetch_add(1, std::memory_order_relaxed);
}

struct Sample {
  uint32_t req = 0, state = 0;  // [0x839400A4] and its +104
  uint32_t job = 0;             // [[0x8394024C]+40]: the request the read-end thread is working on
  uint32_t queue = 0;           // [0x839400A8]: the queue head
  uint32_t pause = 0;           // [0x8362A0B0]: the loader pause flag
  uint64_t opens = 0, reads = 0, completions = 0;
};

struct LoadWatch {
  Sample last;
  int64_t since_us = 0, last_us = 0;
  uint64_t since_frame = 0;
  bool fired = false;

  static bool Same(const Sample& a, const Sample& b) {
    return a.req == b.req && a.state == b.state && a.job == b.job && a.queue == b.queue && a.opens == b.opens &&
           a.reads == b.reads && a.completions == b.completions;
  }
  // frame = the guest frame counter. Returns true ONCE per stretch of no progress (re-arms when anything moves).
  // Guards: nothing loading (req, job and queue all 0: idle menu) or the pause flag set never fires and resets; a gap of more than
  // gap_us since the previous call means THIS thread was not running (laptop sleep, Android freeze, debugger): the clock restarts;
  // and at least min_frames guest frames must have passed since the last progress (the bug's signature is "the UI keeps painting").
  bool Update(const Sample& s, uint64_t frame, int64_t now_us, int64_t limit_us, uint64_t min_frames = 100,
              int64_t gap_us = 2'000'000) {
    const bool stopped = last_us != 0 && now_us - last_us > gap_us;
    last_us = now_us ? now_us : 1;
    const bool loading = s.req != 0 || s.job != 0 || s.queue != 0;
    if (!loading || s.pause != 0 || stopped || !Same(s, last)) {
      last = s;
      since_us = now_us, since_frame = frame;
      fired = false;
      return false;
    }
    if (!fired && limit_us > 0 && now_us - since_us > limit_us && frame - since_frame >= min_frames) {
      fired = true;
      return true;
    }
    return false;
  }
  int64_t HeldUs(int64_t now_us) const { return now_us - since_us; }
  uint64_t FramesHeld(uint64_t frame) const { return frame - since_frame; }
};

}  // namespace wwe2k14::load_probe
