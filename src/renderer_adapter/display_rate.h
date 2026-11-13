// lane 168 (S101, GATE_FAULTS row 4: "120 on a 60 Hz panel"): the rate the frame pump's in-between pictures (the tween) run at follows
// what the display can show, not only the fps setting. Two parts:
//   rate::Decide   pure, no includes beyond <cmath>/<cstdint>; unit-tested by src/renderer_adapter/display_rate_test.cpp
//                  (clang++ -std=c++17 -DWWE2K14_RATE_PURE -I src/renderer_adapter src/renderer_adapter/display_rate_test.cpp)
//   rate::PanelHz  the platform read. Windows: the game window's monitor, its current mode. Android: ONLY the number GameActivity.java
//                  passes (--wwe2k14_panel_peak_hz = the fastest supported display mode at the current size, the one the 120 mode asks
//                  the window for, commit 07f45ae7). SDL lists no display modes on Android and its current-mode read says 60 on the
//                  OPPO at times (s100ph3 / ph5), so nothing is read from SDL there. No number = 0 = unknown = the request is kept.
// The game keeps its 60 updates a second; the decoupled "120" is one halfway picture per tick, so the only two useful pump rates are
// 60 (no tween) and 120 (tween). Decide returns one of them.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rdr_adapter::rate {

struct Decision {
  uint32_t pump_hz = 60;   // 60: no halfway pictures; 120: one halfway picture per game tick
  bool tween = false;
  bool panel_known = false;
  double panel_hz = 0.0;   // the reading used (0 when unknown or rejected)
  const char* why = "";
};

// A refresh reading is plausible between 24 and 1000 Hz (and finite). Anything else is a wrong read (a 0 from a failed query, a bogus
// 9999 from a driver, NaN) and counts as unknown.
inline bool PlausibleHz(double hz) { return std::isfinite(hz) && hz >= 24.0 && hz <= 1000.0; }

// Android's reading: only the passed peak (--wwe2k14_panel_peak_hz); none / non-positive = 0 = unknown.
inline double AndroidPanelHz(int peak_hz) { return peak_hz > 0 ? double(peak_hz) : 0.0; }

// requested: the fps setting (--wwe2k14_step_hz). panel_hz: the display's refresh reading, 0 = could not be read.
// requested <= 60: nothing to decide (60).
// requested > 60 (120, or the monitor's own rate such as 144 when the launcher says UNCAPPED): 120 when the panel shows at least 120
// (1% slack so 119.88 Hz counts), else 60. 144 Hz panels pump at 120 (the halfway picture is exactly half a tick; 144 is not a multiple
// of 60 and a third picture per tick does not exist). A 90 Hz panel drops to 60 (120 pictures a second cannot reach it evenly).
// UNKNOWN reading: the request is kept (120), i.e. today's behaviour. Reason: clamping on a bad read would take 120 away from a
// 120 Hz panel for good, while keeping it only costs the fault this lane fixes, on a panel whose rate we could not read.
inline Decision Decide(double panel_hz, int requested) {
  Decision d;
  if (requested <= 60) {
    d.why = "setting is 60 or less";
    return d;
  }
  if (!PlausibleHz(panel_hz)) {
    d.pump_hz = 120, d.tween = true;
    d.why = "refresh unknown or implausible: request kept";
    return d;
  }
  d.panel_known = true, d.panel_hz = panel_hz;
  if (panel_hz >= 120.0 * 0.99) {
    d.pump_hz = 120, d.tween = true;
    d.why = "panel shows 120 or more";
  } else {
    d.why = "panel cannot show 120: no halfway pictures";
  }
  return d;
}

}  // namespace rdr_adapter::rate

#if !defined(WWE2K14_RATE_PURE)
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rdr_adapter::rate {

// The display's refresh in Hz, 0 = could not be read. window: the game window (Windows HWND; ignored on Android).
// android_peak_hz: --wwe2k14_panel_peak_hz (Android only; 0 = not passed).
inline double PanelHz(void* window, int android_peak_hz = 0) {
#if defined(_WIN32)
  HMONITOR monitor = window ? MonitorFromWindow(HWND(window), MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0.0;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0.0;
  return dm.dmDisplayFrequency > 1 ? double(dm.dmDisplayFrequency) : 0.0;  // 0 / 1 = "hardware default" in Windows' own words
#elif defined(__ANDROID__)
  (void)window;
  return AndroidPanelHz(android_peak_hz);
#else
  (void)window, (void)android_peak_hz;
  return 0.0;
#endif
}

}  // namespace rdr_adapter::rate
#endif  // !WWE2K14_RATE_PURE
