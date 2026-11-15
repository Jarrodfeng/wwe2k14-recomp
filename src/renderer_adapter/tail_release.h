// S102 U1 (docs\renderer\SWAP_TAIL.md): our end-of-frame work moved off the update thread's critical path. Switch wwe2k14_rdr_tail_release (PerFrame,
// default off). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, NO game or SDK includes (the offline test drives it).
//
// The game's render-command (swap) thread, per iteration of sub_82BB5CC0: release the barrier clients (sub_82C3BB38 at 0x82BB5DE4) -> the walk ->
// wait for the clients (sub_82C3BAE0, returns at 0x82BB5F60: every client has ARRIVED, i.e. finished recording) -> list rotation -> the present
// (sub_82BA84E8 -> sub_82CB37C0, where reader.h OnPresent runs, then VdSwap) -> back to the release. Today all of OnPresent runs between the arrival
// and the release, so the update thread (CGameMainThread) waits for it. This splits it:
//   LATCH  (OnPresent, inside the [arrival, release) window): everything that reads state a game thread writes (the scene contributors' Latch,
//          the frame's tables detached from g_frame, the flags the update thread's hooks read);
//   FINISH (right after the release, on the same swap thread; or, as a safety net, at the next walk / the next present): the record's formatting
//          and sinks, the frame's hand-off to the render thread, the geometry forget, the census lines.
// The game's own order (rotation, present, VdSwap, the frame counter, the release) is untouched: only OUR work moves.
//
// The window is the generation stamp: OnArrive opens it, OnRelease closes it and bumps `generation`. A latch is allowed only inside the window
// (the update thread provably parked); a latch outside it is counted (`late_latch`) and never deferred.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rdr_adapter::tail {

enum Why : uint8_t {
  kOff = 0,        // the switch is off
  kNotLive,        // no live mirror (nothing is handed over)
  kCapture,        // a capture writes this frame (its writer stays on the old path)
  kCaseRing,       // the case ring needs the frame's bytes and timing at VdSwap: not deferred
  kLiveSkip,       // debug_live_skip & 4: the frame never reaches the sink
  kOutsideWindow,  // the present is not between the barrier's arrival and its release (the pre-loop present, a present after the release)
  kOtherThread,    // review F5: inside the window but not on the thread that arrived (the swap thread): another thread's present is never deferred
  kWhyCount
};
inline const char* WhyName(uint8_t w) {
  static const char* const k[] = {"off", "not_live", "capture", "case_ring", "live_skip", "outside_window", "other_thread"};
  return w < kWhyCount ? k[w] : "?";
}

// Where a pending finish ran. kAtLock (review F1): a texture lock read-back / an attire pack (a game thread) needed frame G handed over first.
enum Where : uint8_t { kAtRelease = 0, kAtWalk, kAtPresent, kAtLock, kWhereCount };

struct Stats {
  uint64_t presents = 0, deferred = 0, emit_deferred = 0;
  uint64_t declined[kWhyCount] = {};
  uint64_t finished[kWhereCount] = {};   // where a pending finish ran (anything but kAtRelease is a safety net taking it)
  uint64_t late_latch = 0;               // a latch asked for with the window closed (the update thread may be running): never deferred
  uint64_t arrivals = 0, releases = 0;
  uint64_t lock_in_window = 0;           // review F1: a read-back lock / pack found a finish pending (it ran it, or waited for the swap thread's)
  uint64_t mutex_waits = 0;              // review F3: reader-lock acquisitions on other threads that found it held while a finish was running
  uint64_t finish_locked_ns = 0, finish_locked_max_ns = 0;   // review F3: the finish's time under the reader lock (the most another thread can wait on it)
};

using FinishFn = void (*)(Where where);

class State {
 public:
  // The barrier wait returned (swap thread, 0x82BB5F60): every client has arrived and is parked until the release.
  void OnArrive() {
    arrive_tid_.store(ThisThread(), std::memory_order_relaxed);   // review F5: the thread that arrived (the swap thread)
    window_.store(true, std::memory_order_release);
    ++stats_.arrivals;
  }
  static uint64_t ThisThread() { return uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id())) | 1u; }
  // The swap thread is about to release the clients (0x82BB5DE4, before the original runs): the window closes, a new generation starts.
  void OnRelease() {
    window_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    ++stats_.releases;
  }
  bool InWindow() const { return window_.load(std::memory_order_acquire); }
  uint64_t Generation() const { return generation_.load(std::memory_order_acquire); }

  // At a present: may this frame's tail be deferred? `why_not` = kOff / kNotLive / ... when the caller already knows it may not (kWhyCount = it may).
  // Counts the answer. A frame that may be deferred but finds the window closed is declined as kOutsideWindow and counted late_latch.
  bool Decide(uint8_t why_not) {
    ++stats_.presents;
    if (why_not < kWhyCount) {
      ++stats_.declined[why_not];
      return false;
    }
    if (!InWindow()) {
      ++stats_.declined[kOutsideWindow];
      ++stats_.late_latch;
      return false;
    }
    if (ThisThread() != arrive_tid_.load(std::memory_order_relaxed)) {   // review F5
      ++stats_.declined[kOtherThread];
      return false;
    }
    return true;
  }
  // The latch made a pending finish (the caller holds its data). `emit` = the scene record's emit was deferred too.
  void SetPending(bool emit) {
    ++stats_.deferred;
    if (emit) ++stats_.emit_deferred;
    latch_generation_ = Generation();
    pending_.store(true, std::memory_order_release);
  }
  bool Pending() const { return pending_.load(std::memory_order_acquire); }
  // Review F1: the finish runs under FinishMutex() from start to end and clears `pending` only when it is done (FinishDone), so a thread that finds it
  // pending (a read-back lock) either runs it or waits for the one running. Lock order: FinishMutex -> the scene writer's -> the reader's.
  std::mutex& FinishMutex() { return finish_mu_; }
  void FinishDone(Where where) {   // FinishMutex held
    ++stats_.finished[where < kWhereCount ? where : kAtRelease];
    finishing_.store(false, std::memory_order_relaxed);
    pending_.store(false, std::memory_order_release);
  }
  void FinishStarted() { finishing_.store(true, std::memory_order_relaxed); }   // FinishMutex held
  bool Finishing() const { return finishing_.load(std::memory_order_relaxed); }
  void CountMutexWait() { ++stats_.mutex_waits; }   // (approximate: other threads; census only)
  void AddLockedNs(uint64_t ns) {
    stats_.finish_locked_ns += ns;
    if (ns > stats_.finish_locked_max_ns) stats_.finish_locked_max_ns = ns;
  }
  // The generation the pending latch was made in: a finish at kAtRelease must see exactly one release since (the planted twin release_first does not).
  uint64_t LatchGeneration() const { return latch_generation_; }

  void SetFinish(FinishFn fn) { finish_.store(fn, std::memory_order_release); }
  // Safety nets (the walk start, a present) and the read-back locks (kAtLock): run a pending finish now, or wait for the one running (it holds
  // FinishMutex until it is done). One relaxed load when nothing is pending.
  void FlushIfPending(Where where) {
    if (!pending_.load(std::memory_order_acquire)) return;
    if (where == kAtLock) ++stats_.lock_in_window;
    if (FinishFn fn = finish_.load(std::memory_order_acquire)) fn(where);
  }

  const Stats& stats() const { return stats_; }
  // One census line (every 600 frames while the switch is on, reader.h), the window's counts.
  std::string CensusLine(uint64_t from, uint64_t to, const Stats& before) const {
    char b[900];
    const Stats& s = stats_;
    std::snprintf(b, sizeof(b),
                  "[rdr][tail_release] frames %llu-%llu: presents %llu, deferred %llu (emit deferred %llu), declined off %llu, not_live %llu, capture %llu, case_ring %llu, "
                  "live_skip %llu, outside_window %llu, other_thread %llu; finished at release %llu, at walk %llu, at present %llu, at a read-back lock %llu; late latch %llu; "
                  "read-back locks inside the window %llu; reader-lock waits on other threads during a finish %llu; finish under the reader lock %.2f ms total, %.3f ms at most; arrivals %llu, releases %llu",
                  (unsigned long long)from, (unsigned long long)to, (unsigned long long)(s.presents - before.presents),
                  (unsigned long long)(s.deferred - before.deferred), (unsigned long long)(s.emit_deferred - before.emit_deferred),
                  (unsigned long long)(s.declined[kOff] - before.declined[kOff]), (unsigned long long)(s.declined[kNotLive] - before.declined[kNotLive]),
                  (unsigned long long)(s.declined[kCapture] - before.declined[kCapture]), (unsigned long long)(s.declined[kCaseRing] - before.declined[kCaseRing]),
                  (unsigned long long)(s.declined[kLiveSkip] - before.declined[kLiveSkip]),
                  (unsigned long long)(s.declined[kOutsideWindow] - before.declined[kOutsideWindow]), (unsigned long long)(s.declined[kOtherThread] - before.declined[kOtherThread]),
                  (unsigned long long)(s.finished[kAtRelease] - before.finished[kAtRelease]), (unsigned long long)(s.finished[kAtWalk] - before.finished[kAtWalk]),
                  (unsigned long long)(s.finished[kAtPresent] - before.finished[kAtPresent]), (unsigned long long)(s.finished[kAtLock] - before.finished[kAtLock]),
                  (unsigned long long)(s.late_latch - before.late_latch), (unsigned long long)(s.lock_in_window - before.lock_in_window),
                  (unsigned long long)(s.mutex_waits - before.mutex_waits), double(s.finish_locked_ns - before.finish_locked_ns) * 1e-6,
                  double(s.finish_locked_max_ns) * 1e-6, (unsigned long long)(s.arrivals - before.arrivals), (unsigned long long)(s.releases - before.releases));
    return b;
  }

 private:
  std::atomic<bool> window_{false};
  std::atomic<bool> pending_{false};
  std::atomic<uint64_t> generation_{0};
  std::atomic<FinishFn> finish_{nullptr};
  std::atomic<bool> finishing_{false};
  std::atomic<uint64_t> arrive_tid_{0};
  std::mutex finish_mu_;
  uint64_t latch_generation_ = 0;
  Stats stats_;   // swap thread (presents can come from elsewhere: counts only)
};

inline State& The() {
  static State* s = new State;   // leaked: a late hook never meets a destroyed state
  return *s;
}

}  // namespace rdr_adapter::tail
