// wwe2k14 - ultrawide (Session 33 item 6). Default off (--wwe2k14_aspect=16:9).
//
// Route 2 of the plan, anamorphic Hor+: the game keeps rendering its 16:9 frame (its EDRAM targets are sized for
// 1280x720), but the scene's draws see k times more horizontally (SDK gpu_wide_factor = k: clip-space x / k for the
// scene draws and the HUD draws; the full-screen post passes are left alone), and the presenter stretches the frame
// to the window (present_letterbox=false). The scene comes out with the same height and more on the sides; the HUD
// and the menus, squeezed by k first, come out as a centred 16:9. Costs horizontal sharpness (the same pixels spread
// k times wider). k = the screen's aspect / (16/9): 21:9 (2560x1080, 3440x1440) = 1.33, 32:9 = 2.
// --wwe2k14_aspect=auto takes the monitor's aspect (or the window's, with --fullscreen=false and a window size),
// 21:9 / 32:9 force one. Known limit (unconfirmed how much it shows): the game still culls to its own 16:9 view, so
// things at the far left / right can be missing.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/system/xmemory.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)

#include "guest_read.h"

REXCVAR_DEFINE_STRING(wwe2k14_aspect, "16:9", "wwe2k14",
                      "Session 33: screen shape. 16:9 = the game's own (default); auto = the monitor's (or the window's "
                      "when windowed); 21:9 / 32:9. Wider than 16:9 shows more of the scene at the sides (Hor+), the "
                      "HUD and menus stay a centred 16:9. See src/ultrawide.h");

namespace wwe2k14::ultrawide {

inline double ScreenAspect() {
  auto flag = [](const char* n) { return std::atoi(rex::cvar::GetFlagByName(n).c_str()); };
  const std::string fs = rex::cvar::GetFlagByName("fullscreen");
  if ((fs == "false" || fs == "0") && flag("window_width") > 0 && flag("window_height") > 0)
    return double(flag("window_width")) / double(flag("window_height"));
#if defined(_WIN32)  // Session 63 (Android port): the phone's screen comes with ANDROID_PLAN F5
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsHeight)
    return double(dm.dmPelsWidth) / double(dm.dmPelsHeight);
#endif
  return 16.0 / 9.0;
}

// Before the graphics system starts (OnPreSetup): the SDK settings, each only if left at default.
inline void Configure() {
  const std::string a = REXCVAR_GET(wwe2k14_aspect);
  double aspect = 16.0 / 9.0;
  if (a == "auto") aspect = ScreenAspect();
  else if (a == "21:9") aspect = 21.0 / 9.0;
  else if (a == "32:9") aspect = 32.0 / 9.0;
  const double k = std::clamp(aspect / (16.0 / 9.0), 1.0, 4.0);
  if (k < 1.01) {
    if (a != "16:9") REXLOG_INFO("[wwe2k14][wide] aspect {} = {:.3f}: not wider than 16:9, nothing changed", a, aspect);
    return;
  }
  auto set = [](const char* name, const std::string& value) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
  };
  set("gpu_wide_factor", std::to_string(k));
  set("present_letterbox", "false");
  REXLOG_INFO("[wwe2k14][wide] aspect {} = {:.3f}: Hor+ factor {:.3f} (gpu_wide_factor {}, present_letterbox {})", a,
              aspect, k, rex::cvar::GetFlagByName("gpu_wide_factor"), rex::cvar::GetFlagByName("present_letterbox"));
}

// The game's frame (VdSwap): outside the in-game sequence (menus, Create modes) every draw is squeezed into a centred
// 16:9 (SDK gpu_wide_all); in game only the scene and the HUD (states.toml seq: 9 = in game).
inline void OnFrame(uint8_t* base, uint64_t frame) {
  static int on = -1;
  if (on == 0 || frame % 15 != 0) return;
  if (on < 0) {
    on = std::strtod(rex::cvar::GetFlagByName("gpu_wide_factor").c_str(), nullptr) > 1.001 ? 1 : 0;
    if (!on) return;
  }
  auto rd = [base](uint32_t a, uint32_t* out) {
    if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
    *out = _byteswap_ulong(v);
    return true;
  };
  uint32_t a = 0, b = 0, seq = 0;
  const bool in_game = rd(0x839400F8, &a) && rd(a, &b) && rd(b + 48, &seq) && seq == 9;
  static int last = -1;
  if (int(in_game) != last) {
    last = int(in_game);
    rex::cvar::SetFlagByName("gpu_wide_all", in_game ? "false" : "true");
  }
}

}  // namespace wwe2k14::ultrawide
