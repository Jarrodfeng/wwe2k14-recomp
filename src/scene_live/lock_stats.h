// Lock cost counters for the census lines (Session 103 overnight, BHF1; docs\research\s103\BH1_threads.md B2 / B3). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// CLAUDE.md "a cost is fitted before it is cut": the two global locks every model call takes (the wrestler class's mutex_ in WantCount, the weapon tracker's mu_ in OnModelCall) and the guest model reader's
// run_mu_ (held across a whole model read) are not cut until a probe has fitted their ms from the counts. This is the counts: calls, how many had to WAIT (try_lock failed = contention), the wait
// time (sum / max) and the time the lock was held (sum / max), all in microseconds in the text. Pure: no game, SDK or renderer includes. Overhead: one try_lock plus two steady_clock reads per guarded call.
//   LockStats st;  { TimedLock<std::mutex> lk(mu, st); ... }   // the guard locks (try first, then blocking) and records at scope exit
//   st.Text() -> "calls N contended C wait_us SUM/MAX held_us SUM/MAX"
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "../cost_counters.h"   // wwe2k14::cost::On(): the dev switch (header-only, no game includes)

namespace rdr_scene::live {

struct alignas(64) LockStats {   // own cache line: the counters of one lock never share a line with another
  std::atomic<uint64_t> calls{0}, contended{0}, wait_ns{0}, wait_max_ns{0}, held_ns{0}, held_max_ns{0};
  static void Max(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
  }
  // One guarded call: `waited_ns` = 0 when the try_lock succeeded; `contended` = it did not.
  void Note(bool was_contended, uint64_t waited_ns, uint64_t held) {
    calls.fetch_add(1, std::memory_order_relaxed);
    if (was_contended) contended.fetch_add(1, std::memory_order_relaxed), wait_ns.fetch_add(waited_ns, std::memory_order_relaxed), Max(wait_max_ns, waited_ns);
    held_ns.fetch_add(held, std::memory_order_relaxed);
    Max(held_max_ns, held);
  }
  std::string Text() const {
    const auto us = [](const std::atomic<uint64_t>& a) { return std::to_string(a.load(std::memory_order_relaxed) / 1000); };
    return "calls " + std::to_string(calls.load(std::memory_order_relaxed)) + " contended " + std::to_string(contended.load(std::memory_order_relaxed)) + " wait_us " + us(wait_ns) + "/" + us(wait_max_ns) + " held_us " + us(held_ns) + "/" + us(held_max_ns);
  }
};

template <class Mutex>
class TimedLock {
 public:
  TimedLock(Mutex& m, LockStats& st) : m_(m), st_(st) {
    using clock = std::chrono::steady_clock;
    if (!wwe2k14::cost::On()) {   // BHF1 fix: ~100 model calls a frame: the default path is a plain lock, no clock reads, no counter writes (the counters are the --wwe2k14_cost_counters dev switch's)
      m_.lock();
      timed_ = false;
      return;
    }
    if (m_.try_lock()) {
      contended_ = false;
      t0_ = clock::now();
    } else {
      contended_ = true;
      const auto a = clock::now();
      m_.lock();
      t0_ = clock::now();
      waited_ = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t0_ - a).count());
    }
  }
  ~TimedLock() {
    if (!timed_) {
      m_.unlock();
      return;
    }
    const uint64_t held = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_).count());
    m_.unlock();
    st_.Note(contended_, waited_, held);
  }
  TimedLock(const TimedLock&) = delete;
  TimedLock& operator=(const TimedLock&) = delete;

 private:
  Mutex& m_;
  LockStats& st_;
  std::chrono::steady_clock::time_point t0_;
  uint64_t waited_ = 0;
  bool contended_ = false, timed_ = true;
};

}  // namespace rdr_scene::live
