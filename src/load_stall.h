// wwe2k14 - loader stall trigger timing (S103, moveset Signature / Finishers infinite loading; docs/research/s103/MVL.md).
// Pure logic, no SDK includes (unit test tools\tests\load_stall_test.cpp). Fed one (request, state) per worker step by
// the sub_82C37070 hook in load_probe.h (always on, S103): Update() returns true ONCE when the same (request, state 9)
// pair has lasted more than limit_us; any change of request or state (9 -> 0 included) resets it. LOG ONLY.
// S103 (always on for players): per worker step the hook does two guest loads, one clock read and these compares, NO
// mutex (LastPath below is lock-free); the dump runs once per run (StallOnce) and never changes behaviour.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace wwe2k14::load_probe {

struct StallWatch {
  uint32_t req = 0, state = 0;
  int64_t since_us = 0, last_us = 0;
  uint64_t since_frame = 0;
  bool fired = false;

  // frame = the guest frame counter. Two guards against a healthy game (S103 review): a gap of more than gap_us since the
  // previous step means the world stopped (laptop sleep, debugger, Android freeze / thaw, a long hitch): the clock
  // restarts; and the real bug's signature is "the UI keeps painting", so at least min_frames guest frames must have
  // passed since the pair began.
  bool Update(uint32_t r, uint32_t st, int64_t now_us, int64_t limit_us, uint64_t frame, int64_t gap_us = 1'000'000,
              uint64_t min_frames = 100) {
    const bool stopped = last_us != 0 && now_us - last_us > gap_us;
    last_us = now_us ? now_us : 1;
    if (r != req || st != state) {
      req = r;
      state = st;
      since_us = now_us;
      since_frame = frame;
      fired = false;
      return false;
    }
    if (stopped) {
      since_us = now_us;
      since_frame = frame;
      return false;
    }
    if (state == 9 && !fired && now_us - since_us > limit_us && frame - since_frame >= min_frames) {
      fired = true;
      return true;
    }
    return false;
  }
  int64_t HeldUs(int64_t now_us) const { return now_us - since_us; }
};

// Newest enqueued pac path, lock-free: one writer at a time is the game thread that enqueues (a rare second writer or a
// reader racing a write can at worst show a torn name in a log line; nothing depends on it). 4 slots of 20 bytes; the
// writer fills the next slot, then publishes its index, so a reader normally sees a whole name.
struct LastPath {
  static constexpr int kSlots = 4, kLen = 20;
  char slot[kSlots][kLen] = {};
  std::atomic<uint32_t> head{0};
  // src: up to 16 guest bytes (the reader's name field); non-printable bytes become '?', stops at NUL.
  void Set(const uint8_t* src) {
    const uint32_t n = (head.load(std::memory_order_relaxed) + 1) % kSlots;
    int i = 0;
    for (; i < 16 && src[i]; ++i) slot[n][i] = (src[i] >= 32 && src[i] < 127) ? char(src[i]) : '?';
    slot[n][i] = 0;
    head.store(n, std::memory_order_release);
  }
  std::string Get() const {
    const uint32_t h = head.load(std::memory_order_acquire) % kSlots;
    char b[kLen];
    for (int i = 0; i < kLen; ++i) b[i] = slot[h][i];
    b[kLen - 1] = 0;
    return std::string(b);
  }
};

// The stack dump is taken for the first stall of a run only; later stalls log one line.
struct StallOnce {
  std::atomic<bool> dumped{false};
  bool TakeDump() { return !dumped.exchange(true); }
};

}  // namespace wwe2k14::load_probe
