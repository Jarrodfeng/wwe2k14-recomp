#pragma once
// S102 lane H2 piece 3 (--wwe2k14_rdr_fence_friendly, default off; R5 item 4): the render thread's wait for its frame slot's fence, without a driver spin.
//
// Renderer::Impl::Render waited with vkWaitForFences(..., UINT64_MAX): 4.6 ms a frame on the Surface (13-17 % of a core, R5). Whether the Intel driver spins inside that call
// is UNCONFIRMED (R5 [U]). This wait does not depend on the answer: it polls vkGetFenceStatus (a user-mode query) for a short spin, then sleeps in slices that grow with the
// time already waited (a slice is an eighth of the wait so far, 0.1-0.5 ms asked; a high-resolution timer wake takes 0.5-1 ms in the offline test, so the real slices are 0.5-1 ms),
// so a 4.6 ms wait costs a handful of wakes and the frame learns of the fence at most one wake late (measured: median 0.38 ms, p90 0.87 ms). After kCapNs the caller falls back to the blocking call (a lost device or a hung GPU keeps its old behaviour and
// its Problem line). No Vulkan or Windows header is needed by the test: the three callables are the fence poll, the clock and the sleep (tools\h2\h2_waits_test.cpp).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// NOUSER / NOGDI: winuser.h defines the macro DrawState, which renderer.cpp uses as a type name.
#ifndef NOUSER
#define NOUSER
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#endif

namespace rdr::fence_wait {

struct Params {
  int64_t spin_ns = 100000;      // polls with a yield before the first sleep
  int64_t slice_min_ns = 100000;
  int64_t slice_max_ns = 500000;
  int64_t cap_ns = 200000000;    // give up (the caller blocks in the driver) after 200 ms
};

// poll() -> true when the fence is signalled; now() -> ns; sleep(ns) -> bool (false = it could not sleep, e.g. no high-resolution timer). Returns true = signalled, false = still
// unsignalled after cap_ns or the sleep failed: the caller then runs the blocking wait.
template <class Poll, class Now, class Sleep>
inline bool Wait(Poll&& poll, Now&& now, Sleep&& sleep, const Params& p = {}) {
  const int64_t t0 = now();
  for (;;) {
    if (poll()) return true;
    const int64_t waited = now() - t0;
    if (waited >= p.cap_ns) return false;
    if (waited < p.spin_ns) {
      sleep(0);  // a yield, not a sleep
      continue;
    }
    if (!sleep(std::clamp<int64_t>(waited / 8, p.slice_min_ns, p.slice_max_ns))) return false;
  }
}

inline int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// One sleep on this thread's high-resolution waitable timer (~0.1-0.5 ms overshoot; the default Sleep is 1 ms or 15.6 ms); 0 = a yield.
inline bool SleepNs(int64_t ns) {
  if (ns <= 0) {
    std::this_thread::yield();
    return true;
  }
#ifdef _WIN32
  struct Timer {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~Timer() { if (h) CloseHandle(h); }
  };
  thread_local Timer timer;
  if (timer.h) {
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100);
    if (SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer.h, INFINITE);
      return true;
    }
  }
  return false;  // no high-resolution timer: do not fall back to Sleep (1-15 ms): the caller's blocking wait runs instead
#else
  std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
  return true;
#endif
}

}  // namespace rdr::fence_wait
