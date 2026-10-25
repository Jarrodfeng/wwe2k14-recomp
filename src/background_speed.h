// wwe2k14 - full speed with the window in the background (Session 32, Jarrod: "when the windows aren't focused they
// run a fair bit slower, can we bypass this for the multi-lane testing?"). Default off; tools\bench\run_list.ps1 turns
// it on for its runs (the lanes open behind other windows since Session 30).
//
// Why they are slower (Microsoft's documented Windows 11 behaviour, not measured here before this change): a process
// whose windows are all occluded or minimised has its timeBeginPeriod request ignored (the timer stays at ~15.6 ms),
// and a background process can be run in EcoQoS (efficiency) mode. The game's update thread sleeps 1 ms many times a
// frame (src\precise_sleep.h, which is off by default, documents the 14 x Sleep(1) loop), so a coarse timer stretches
// every frame. SetProcessInformation(ProcessPowerThrottling) with both controls set and both states clear opts the
// process out of both: its 1 ms timer request is honoured whatever its window's state, and it is never throttled.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)

REXCVAR_DEFINE_BOOL(wwe2k14_background_full_speed, false, "wwe2k14",
                    "Session 32: opt the process out of Windows' background power throttling (EcoQoS, and the coarse "
                    "timer for occluded windows), so a game behind other windows runs as fast as a focused one. "
                    "run_list turns it on for test runs");

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace wwe2k14 {

inline void ApplyBackgroundFullSpeed() {
  if (!REXCVAR_GET(wwe2k14_background_full_speed)) return;
#if !defined(_WIN32)
  REXLOG_INFO("[wwe2k14] background full speed: Windows power throttling only (nothing to do here)");  // Session 63
#else
  PROCESS_POWER_THROTTLING_STATE s{};
  s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  s.StateMask = 0;  // both off: never EcoQoS, the timer resolution request always honoured
  const BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
  if (ok)
    REXLOG_INFO("[wwe2k14] background full speed: power throttling and the occluded-window timer limit are off");
  else
    REXLOG_WARN("[wwe2k14] background full speed: SetProcessInformation failed (error {})", GetLastError());
#endif
}

}  // namespace wwe2k14
