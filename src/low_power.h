// Session 37: low power for laptops (the brief: on a laptop the CPU and the integrated GPU share one power and heat
// budget; CPU threads that spin may cost the GPU frame rate on 15 W machines).
//
// Every CPU-side GPU wait of the game (docs\systems\gpu_sync.md: the frame-pacing wait for the previous frame's flip
// on every present, ring room, heap reuse, fences) is a loop around one poll step, sub_82CB1F50 (gpu_wait_poll_step,
// db/gpu_sync.json, two labels). On the Xbox 360 that step lowers the hardware thread's priority (cctpl), waits
// 32 x db16cyc and raises it again (cctpm): the core's other hardware thread gets the time. Recompiled, those
// instructions compile to nothing, so the game's main thread busy-spins for the whole wait (Session 4: 52% of the
// main thread at the title, ~75% in slow entrance windows).
//
// --wwe2k14_low_power (default ON since 2026-10-20 ~12:40, Jarrod: "let's make it default", after the full regress on
// vs off: same pacing; --wwe2k14_low_power=false restores the spinning): when the step says "keep waiting" (r3 = 1),
// the thread gives the core back:
// the first --wwe2k14_low_power_spin_us of a wait with the CPU's pause hint (what db16cyc was for), after that in
// short high-resolution timer sleeps (--wwe2k14_low_power_sleep_us, 250 us: small against a 16.7 ms frame). The
// step itself (its fence and hung-flag checks) runs unchanged; only the time between steps changes.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <immintrin.h>

REXCVAR_DEFINE_BOOL(wwe2k14_low_power, true, "wwe2k14",
                    "Session 37: the game's GPU waits (gpu_wait_poll_step) give the CPU core back instead of "
                    "busy-spinning (pause, then short timer sleeps), and the GPU command / UI threads stop spinning "
                    "(present_ui_idle_wait, gpu_wait_reg_mem_sleep_us). Default on (Jarrod, 2026-10-20); false = spin")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wwe2k14_low_power_spin_us, 100, "wwe2k14",
                     "Session 37: with --wwe2k14_low_power, how long a GPU wait spins (pause hint) before it sleeps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(wwe2k14_low_power_sleep_us, 250, "wwe2k14",
                     "Session 37: with --wwe2k14_low_power, the length of each sleep in a long GPU wait (high-resolution "
                     "timer)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace wwe2k14::low_power {

struct ThreadWait {
  bool waiting = false;
  LARGE_INTEGER start{};
  HANDLE timer = nullptr;
};
inline thread_local ThreadWait t_wait;
inline std::atomic<uint64_t> g_sleeps{0}, g_steps{0};
inline LARGE_INTEGER Freq() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v;
  }();
  return f;
}

// After a poll step that said "keep waiting".
inline void Between() {
  ThreadWait& w = t_wait;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!w.waiting) w.waiting = true, w.start = now;
  g_steps.fetch_add(1, std::memory_order_relaxed);
  const int64_t elapsed_us = (now.QuadPart - w.start.QuadPart) * 1000000 / Freq().QuadPart;
  if (elapsed_us < REXCVAR_GET(wwe2k14_low_power_spin_us)) {
    for (int i = 0; i < 16; ++i) _mm_pause();
    return;
  }
  if (!w.timer) {
    w.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!w.timer) w.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // before Windows 10 1803
  }
  if (w.timer) {
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(std::max(REXCVAR_GET(wwe2k14_low_power_sleep_us), 1)) * 10;  // 100 ns units, relative
    if (SetWaitableTimer(w.timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(w.timer, INFINITE);
      g_sleeps.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  SwitchToThread();
}

// At gpu_wait_begin (sub_82CB1E58): a new wait starts with its spin again.
inline void Begin() { t_wait.waiting = false; }

// OnPreSetup: the two host threads that spun too (run 2622, thread_busy: ~95-100% of a core each in a match), each
// only if left at default: the UI thread between guest frames (SDK present_ui_idle_wait) and the GPU command thread in
// WAIT_REG_MEM (SDK gpu_wait_reg_mem_sleep_us: short precise sleeps instead of yielding).
inline void Configure() {
  if (!REXCVAR_GET(wwe2k14_low_power)) return;
  for (auto [name, value] : {std::pair{"present_ui_idle_wait", "true"}, std::pair{"gpu_wait_reg_mem_sleep_us", "100"}})
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
  REXLOG_INFO("[wwe2k14][low_power] on: the game's GPU waits sleep after {} us (sleeps of {} us), present_ui_idle_wait={}, "
              "gpu_wait_reg_mem_sleep_us={}",
              REXCVAR_GET(wwe2k14_low_power_spin_us), REXCVAR_GET(wwe2k14_low_power_sleep_us),
              rex::cvar::GetFlagByName("present_ui_idle_wait"), rex::cvar::GetFlagByName("gpu_wait_reg_mem_sleep_us"));
}

}  // namespace wwe2k14::low_power

// Called from src\gpu_timeline.h's hooks: the mid-asm hook at gpu_wait_begin (Begin) and REX_HOOK_RAW(sub_82CB1F50)
// (Between, after a step that returned 1). One hook per guest function, so they live there.
