#pragma once
// wwe2k14 - Session 101 lane 306: the "Owner timing" thread's waits, with no game or SDK header in them (tools\owner\spin_cpu_test.cpp runs this shipped code).
//
// HiResWait::Until(target, spin) sleeps on a high-resolution waitable timer to `spin` before the target and spins only that last stretch.
// THE TRAP (found by reading in lane 306 from Surface runs 181-184): the old rule "arm the timer only if target - now > spin + 100 us, else spin to the target" spins the WHOLE
// interval for any target closer than the spin window. The gate poll (target = now + 250 us, spin 1 ms) was exactly that: with the flip gate open (from every flip to the next
// vblank, most of each frame) the thread spun 45-70 % of a core on the Surface (4C/8T, where the render thread is the bottleneck). Rule now: a target that is a POLL, not a
// deadline, gets spin 0 (PlanWait below), and no spin window exceeds kVblankSpinNs, so a spin is at most ~0.6 ms a vblank.
#include <algorithm>
#include <chrono>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <time.h>
#endif

namespace wwe2k14::owner_wait {

inline constexpr int64_t kVblankSpinNs = 500000;  // the spin before a real vblank deadline (was 1 ms: 6 % of a core at 60 Hz by itself)
inline constexpr int64_t kTimerSlackNs = 500000;  // the largest overshoot we allow a high-resolution timer wake before the deadline spin window starts
inline constexpr int64_t kGatePollNs = 1000000;   // the flip gate's poll when the vblank is ours (was 250 us). Our own Vblank() runs the handler that clears the flip word and
                                                  // re-reads the gate at once, so this poll is only a safety net for a writer we do not know of; it never spins.

// This thread's CPU use over a window. Windows: delta QueryThreadCycleTime(this thread) / delta __rdtsc() = the share of one core, no calibration (GetThreadTimes is
// tick-quantised and misreads a thread that wakes 60-240 times a second: lane 306 review probe, 60 Hz read 5.94 % for a real 1.42 %). Elsewhere (Android):
// CLOCK_THREAD_CPUTIME_ID.
struct CpuMeter {
  int64_t a0 = 0, b0 = 0;
  static void Sample(int64_t& a, int64_t& b) {
#ifdef _WIN32
    ULONG64 cyc = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    a = int64_t(cyc);
    b = int64_t(__rdtsc());
#else
    timespec ts;
    a = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec : 0;
    b = 0;
#endif
  }
  CpuMeter() { Sample(a0, b0); }
  // CPU ms used since the last Take (or construction); window_ms = the wall time of the window (used on Windows; elsewhere the delta is already time).
  double Take(double window_ms) {
    int64_t a, b;
    Sample(a, b);
    const int64_t da = a - a0, db = b - b0;
    a0 = a;
    b0 = b;
#ifdef _WIN32
    return db > 0 ? window_ms * double(da) / double(db) : 0.0;
#else
    (void)window_ms;
    return double(da) / 1.0e6;
#endif
  }
};

// One wait: sleep to `target`, spinning only `spin` ns of it. The old (pre-lane-306) plan is kept in the test as the "before" arm.
struct WaitPlan {
  int64_t target_ns;
  int64_t spin_ns;
};
// The thread's idle wait when the vblank is ours. deadline = the next vblank; gate_open = a flip is pending. The gate poll is a poll (spin 0); the vblank deadline is
// the only target that spins, and only for kVblankSpinNs (not at all for intervals under 4 ms: vsync off at 1 kHz).
inline WaitPlan PlanWait(int64_t now_ns, int64_t deadline_ns, bool gate_open, int64_t interval_ns) {
  if (gate_open && now_ns + kGatePollNs + kVblankSpinNs + kTimerSlackNs < deadline_ns) return {now_ns + kGatePollNs, 0};  // (a poll that could wake into the deadline spin window is skipped)
  // The spin scales with the interval (240 Hz at a flat 0.5 ms would cost 5.5 % of a core): at most 1/32 of it.
  return {deadline_ns, interval_ns >= 4000000 ? std::min(kVblankSpinNs, interval_ns / 32) : 0};
}

// S102 H2 piece 1 (--wwe2k14_owner_load_batch, default off; R5 item 6): the load-turbo windows run the vsync-off vblank at wwe2k14_load_turbo_vblank_hz: 16 kHz by default,
// 4 kHz (250 us) under the lean set that every default player config gets (TRUE NATIVE -> d3d_owned -> s95_lean; the Surface logs show interval 250 us). At those intervals
// the thread wakes 4-16 thousand times a second; whatever the timer does not cover is a spin (R5: 60-97 % of a core for 3-4 s at each match load). Batched: while the in-game
// load window is open, for intervals under kBatchBelowNs, the thread sleeps on the timer until kBatchWakeNs after its last wake (a wake costs about 1 ms of wall time, measured
// offline: ~1.03 ms at 16 kHz, so ~16 vblanks per wake) and the clock owes it the vblanks that fell due (all dispatched in one Turn: the count is unchanged, each vblank at
// most a wake late: p90 0.94 ms, max 1.6 ms in the offline test). The catch-up limit (CatchUpFor) is raised only for those intervals, only while batching: the default of 8 would
// read a batch as a stall and drop vblanks, and a limit of 256 at 60 Hz would fire ~120 interrupts back to back after a 2 s stall. An item (a flip, a fence) still wakes the
// wait at once, and no batching while a flip is pending (BatchEnabled: the frame limiter's gate would wait a wake per load frame).
inline constexpr int64_t kBatchBelowNs = 1000000;  // intervals under this (above 1 kHz: 4 kHz and 16 kHz) are the load-turbo rates
inline constexpr int64_t kBatchWakeNs = 500000;    // at least this long between idle wakes
inline constexpr uint32_t kBatchCatchUp = 256;     // VblankClock::max_catch_up while batching: a timer wake late by a few ms on a busy 4-core machine owes ~100 (16 ms), caught up in one Turn
inline constexpr uint32_t kDefaultCatchUp = 8;     // VblankClock::kMaxCatchUp
inline uint32_t CatchUpFor(bool batch, int64_t interval_ns) { return batch && interval_ns > 0 && interval_ns < kBatchBelowNs ? kBatchCatchUp : kDefaultCatchUp; }
inline bool BatchEnabled(bool switch_on, bool ingame_window_open, bool gate_open) { return switch_on && ingame_window_open && !gate_open; }
inline WaitPlan PlanWaitBatched(int64_t now_ns, int64_t deadline_ns, bool gate_open, int64_t interval_ns, int64_t last_wake_ns, bool enabled) {
  const WaitPlan p = PlanWait(now_ns, deadline_ns, gate_open, interval_ns);
  if (!enabled || interval_ns <= 0 || interval_ns >= kBatchBelowNs) return p;
  return {std::max(p.target_ns, last_wake_ns + kBatchWakeNs), 0};
}

struct HiResWait {
#ifdef _WIN32
  HANDLE timer = nullptr, ev = nullptr;
  bool Init() {
    if (!ev) ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return ev && timer;
  }
  bool Available() const { return ev && timer; }
  void Signal() const { if (ev) SetEvent(ev); }
  void WaitEvent() const { if (ev) WaitForSingleObject(ev, INFINITE); }  // no deadline (after RequestStop: only an item or the stop wakes it)
  static int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  // Until `target_ns` (steady_clock ns); `wake_on_item`: the queue event ends it early. Returns at the target (or early on the event). At most spin_ns + 100 us is spent
  // spinning (when the target is nearer than that the spin is the rest of the interval: bounded by the caller's choice of spin, 0 for a poll).
  void Until(int64_t target_ns, int64_t spin_ns, bool wake_on_item) const {
    const int64_t rem = target_ns - NowNs();
    if (rem > spin_ns + 100000) {
      LARGE_INTEGER due;
      due.QuadPart = -((rem - spin_ns) / 100);
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      HANDLE h[2] = {timer, ev};
      const DWORD r = WaitForMultipleObjects(wake_on_item ? 2 : 1, h, FALSE, INFINITE);
      if (r == WAIT_OBJECT_0 + 1) {
        CancelWaitableTimer(timer);
        return;
      }
    }
    while (NowNs() < target_ns) {
      if (wake_on_item && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0) return;
      YieldProcessor();
    }
  }
#else
  bool Init() { return false; }
  bool Available() const { return false; }
  void Signal() const {}
  void WaitEvent() const {}
  static int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  void Until(int64_t, int64_t, bool) const {}  // (never called: Available() is false)
#endif
};

}  // namespace wwe2k14::owner_wait
