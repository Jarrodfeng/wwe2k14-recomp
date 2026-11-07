// Session 95 (warm 60 route #7): during fullscreen play the desktop switches to 1280x720, so the final paint is a
// 1:1 copy (no scale to the panel's native size; on the Surface Pro 6 that is 2736x1824: expected -1.2 ms and -33 MB a
// paint, both estimates, not measured). Off by default (--wwe2k14_display_720p, the launcher's Advanced row
// "720P DISPLAY"). Meant for integrated graphics; the player turns it on, nothing here checks the GPU.
//
// How it is undone (the point of the design):
//  - ChangeDisplaySettingsEx with CDS_FULLSCREEN is a TEMPORARY mode: it never writes the registry mode, so a reboot or
//    a sign-out always returns to the panel's native mode, and Windows restores it when the process ends however it ends
//    (crash, TerminateProcess, the run lock's kill). That OS rule is unsourced here and has not
//    been seen on the Surface in this project (unconfirmed: the taskkill test decides).
//  - Our own restore on top of it, so the desktop is back at once and not at the OS's pace: window close and script
//    quit (OnWindowCloseRequested and g_request_quit; ReXApp::OnClosing is private), focus lost (alt-tab, minimise, a popup:
//    restore; focus gained: switch again), an unhandled exception and abort() (crash_handler.h).
//  - Never touches a mode that is already 720 or lower, never runs windowed (--fullscreen=false) or on a secondary
//    monitor (--monitor), never on a mode the display does not list (it logs the list and the nearest panel-aspect mode
//    instead, and changes nothing).
//
// Runs once in OnPreSetup, before the window exists, so the window opens fullscreen straight at the new size (SDL's
// borderless fullscreen takes the desktop's size) and the presenter's first swapchain is 1280x720.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)

REXCVAR_DEFINE_BOOL(wwe2k14_display_720p, false, "wwe2k14",
                    "Session 95: in fullscreen, switch the display to wwe2k14_display_720p_mode while the game has focus "
                    "(a temporary mode: restored on close, alt-tab and crash), so the final paint is a 1:1 copy with no "
                    "scale to the panel's native size. For integrated graphics. Windows only; default off");
REXCVAR_DEFINE_STRING(wwe2k14_display_720p_mode, "1280x720", "wwe2k14",
                      "Session 95: the WIDTHxHEIGHT wwe2k14_display_720p switches to (it must be a mode the display "
                      "lists; the log names the list and the nearest panel-aspect mode when it is missing)");

namespace wwe2k14::display_mode {

#if defined(_WIN32)

inline std::atomic<bool> g_wanted{false};   // a mode was chosen and applied once (focus gained re-applies it)
inline std::atomic<bool> g_active{false};   // the desktop is in our mode right now
inline DEVMODEW g_mode{};                   // the chosen mode (the entry the display listed)
inline int g_native_w = 0, g_native_h = 0;  // the desktop's mode before us
inline std::atomic<int> g_restores{0}, g_reapplies{0};

// The display's own restore: no mode + flags 0 = back to the registry (native) mode. Safe from a crash handler (no
// logging, no allocation).
inline LONG RestoreQuiet() {
  if (!g_active.exchange(false)) return DISP_CHANGE_SUCCESSFUL;
  return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
}

// Crash handlers: give the desktop back and never take it again (the UI thread may still be pumping focus events).
inline void RestoreForGood() {
  g_wanted = false;
  RestoreQuiet();
}

inline bool ApplyMode() {
  DEVMODEW dm = g_mode;
  const LONG r = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
  if (r != DISP_CHANGE_SUCCESSFUL) {
    REXLOG_WARN("[display] switching to {}x{} failed (ChangeDisplaySettingsEx {}); the desktop stays as it was",
                int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(r));
    return false;
  }
  g_active = true;
  return true;
}

// OnPreSetup, after the settings are final. Logs one "[display]" line either way.
inline void Configure() {
  if (!REXCVAR_GET(wwe2k14_display_720p)) return;
  // The launcher's precompile run is a background child that never gets focus: leave the desktop alone.
  if (!rex::cvar::GetFlagByName("wwe2k14_precompile").empty()) return;
  if (rex::cvar::GetFlagByName("fullscreen") != "true") {
    REXLOG_INFO("[display] 720p display: windowed run, nothing to switch");
    return;
  }
  if (const std::string mon = rex::cvar::GetFlagByName("monitor"); !mon.empty() && mon != "0" && mon != "1") {
    REXLOG_INFO("[display] 720p display: --monitor={} (a secondary display), not switched", mon);
    return;
  }
  int want_w = 0, want_h = 0;
  if (std::sscanf(REXCVAR_GET(wwe2k14_display_720p_mode).c_str(), "%dx%d", &want_w, &want_h) != 2 || want_w < 640 ||
      want_h < 360) {
    REXLOG_WARN("[display] 720p display: '{}' is not WIDTHxHEIGHT, not switched", REXCVAR_GET(wwe2k14_display_720p_mode));
    return;
  }
  DEVMODEW cur{};
  cur.dmSize = sizeof(cur);
  if (!EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &cur, 0)) {
    REXLOG_WARN("[display] 720p display: the current display mode cannot be read, not switched");
    return;
  }
  g_native_w = int(cur.dmPelsWidth), g_native_h = int(cur.dmPelsHeight);
  if (int(cur.dmPelsWidth) <= want_w && int(cur.dmPelsHeight) <= want_h) {
    REXLOG_INFO("[display] 720p display: the desktop is already {}x{}, nothing to do", g_native_w, g_native_h);
    return;
  }
  // The display's listing: the wanted size at the current colour depth, the current refresh rate first, else the closest
  // one below it, else the closest above.
  std::vector<DEVMODEW> all;
  for (DWORD i = 0;; ++i) {
    DEVMODEW m{};
    m.dmSize = sizeof(m);
    if (!EnumDisplaySettingsExW(nullptr, i, &m, 0)) break;
    if (m.dmBitsPerPel == cur.dmBitsPerPel) all.push_back(m);
  }
  const DEVMODEW* best = nullptr;
  for (const DEVMODEW& m : all) {
    if (int(m.dmPelsWidth) != want_w || int(m.dmPelsHeight) != want_h) continue;
    // Prefer the refresh rate nearest the current one (the driver decides between centred / stretched twins).
    auto score = [&](const DEVMODEW& x) {
      const int df = int(x.dmDisplayFrequency) - int(cur.dmDisplayFrequency);
      return (df <= 0 ? 0 : 1000) + std::abs(df);
    };
    if (!best || score(m) < score(*best)) best = &m;
  }
  if (!best) {
    // Say what is there: the distinct sizes up to 1920 wide, and the one nearest the panel's aspect that is not above
    // 1400 wide (the cheapest 3:2-ish step down; a different scale of the same picture, not a 1:1 copy of 1280x720).
    std::vector<std::pair<int, int>> sizes;
    for (const DEVMODEW& m : all) {
      if (m.dmPelsWidth > 1920) continue;
      const std::pair<int, int> s{int(m.dmPelsWidth), int(m.dmPelsHeight)};
      if (std::find(sizes.begin(), sizes.end(), s) == sizes.end()) sizes.push_back(s);
    }
    std::string list;
    for (auto& s : sizes) list += (list.empty() ? "" : " ") + std::to_string(s.first) + "x" + std::to_string(s.second);
    const double native_aspect = double(g_native_w) / double(std::max(1, g_native_h));
    std::pair<int, int> near_s{0, 0};
    double near_d = 1e9;
    for (auto& s : sizes) {
      if (s.first > 1400) continue;
      const double d = std::abs(double(s.first) / double(s.second) - native_aspect);
      if (d < near_d || (d == near_d && s.first > near_s.first)) near_d = d, near_s = s;
    }
    REXLOG_WARN("[display] 720p display: {}x{} is not in the display's mode list (native {}x{}); not switched. "
                "Modes up to 1920 wide: [{}]. Nearest the panel's aspect up to 1400 wide: {}",
                want_w, want_h, g_native_w, g_native_h, list,
                near_s.first ? std::to_string(near_s.first) + "x" + std::to_string(near_s.second) : std::string("none"));
    return;
  }
  if (int(best->dmDisplayFrequency) != int(cur.dmDisplayFrequency)) {
    REXLOG_WARN("[display] 720p display: the {}x{} mode lists {} Hz, the desktop runs {} Hz; the refresh rate changes "
                "(the launcher's step rate was planned from the old one)",
                want_w, want_h, int(best->dmDisplayFrequency), int(cur.dmDisplayFrequency));
  }
  g_mode = *best;
  g_mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
  if (!ApplyMode()) return;
  g_wanted = true;
  REXLOG_INFO("[display] 720p display: {}x{} -> {}x{} @ {} Hz (a temporary mode: restored on close, alt-tab and crash)",
              g_native_w, g_native_h, int(g_mode.dmPelsWidth), int(g_mode.dmPelsHeight), int(g_mode.dmDisplayFrequency));
}

// Window close, script quit and OnShutdown. Logs the result and the size read back, so a failed restore shows.
inline void Restore(const char* why) {
  if (!g_active.load()) return;
  const LONG r = RestoreQuiet();
  g_restores++;
  DEVMODEW now{};
  now.dmSize = sizeof(now);
  const bool ok = EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &now, 0) != 0;
  if (r != DISP_CHANGE_SUCCESSFUL) {
    REXLOG_WARN("[display] 720p display: restore FAILED (ChangeDisplaySettingsEx {}), desktop reads {}x{} ({})", int(r),
                ok ? int(now.dmPelsWidth) : 0, ok ? int(now.dmPelsHeight) : 0, why);
    return;
  }
  REXLOG_INFO("[display] 720p display: desktop restored to {}x{} ({}); reads back {}x{}", g_native_w, g_native_h, why,
              ok ? int(now.dmPelsWidth) : 0, ok ? int(now.dmPelsHeight) : 0);
}

// Wwe2k14App::OnWindowFocusChanged (UI thread): leave the mode when the game loses focus, take it again on return.
// Returns true when the re-apply failed: the caller re-fits the window (the mode is given up for the rest of the run).
inline bool OnFocus(bool focused) {
  if (!g_wanted.load()) return false;
  if (!focused) {
    Restore("focus lost");
  } else if (!g_active.load()) {
    if (ApplyMode()) {
      g_reapplies++;
      REXLOG_INFO("[display] 720p display: {}x{} again (focus back)", int(g_mode.dmPelsWidth), int(g_mode.dmPelsHeight));
    } else {
      g_wanted = false;
      REXLOG_WARN("[display] 720p display: re-apply failed, the mode is given up for this run");
      return true;
    }
  }
  return false;
}

#else  // not Windows: nothing to switch
inline void Configure() {}
inline void RestoreQuiet() {}
inline void RestoreForGood() {}
inline void Restore(const char*) {}
inline bool OnFocus(bool) { return false; }
#endif

}  // namespace wwe2k14::display_mode
