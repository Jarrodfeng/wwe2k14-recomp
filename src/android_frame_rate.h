#pragma once
// Session 73 (Jarrod's idea 8 of the third ten: smooth frame timing on 90 / 120 Hz phone screens). The game makes a
// frame on its own 60 Hz vblank clock (30 fps frames every other one), but nothing told Android that: a phone left at
// 90 Hz shows 60 fps frames for 1, 2, 1, 2 ... refreshes (11 / 22 ms, an even judder), and the screen may change its
// rate under the game (adaptive refresh). ANativeWindow_setFrameRateWithChangeStrategy (Android 12; Android 11's
// ANativeWindow_setFrameRate without the strategy) says "a fixed 60 fps source": SurfaceFlinger then runs the screen
// at a multiple of 60 (60 or 120 Hz) while the game is in front. Always 60: menus, matches and 60 fps entrances run at
// 60, and 30 fps frames divide 60 and 120 evenly too, so the screen never switches mid-game.
// Session 95 (PACE): the declared rate follows the SETTING, never the frame word: 120 only when the game's step is 120 (the launcher's
// FRAME RATE 120: --wwe2k14_step_hz=120, or the decoupled 120), otherwise 60. A change of the setting re-declares (the call is
// non-seamless, so it is made once per new window and once per change, never per frame).
// Set once per window (Android makes a new ANativeWindow when the app comes back from the background) and logged with
// the screen's rate before and a few seconds after: "[wwe2k14][frame_rate]". Functions by dlsym (the APK's minimum is
// Android 9). --wwe2k14_android_frame_rate=false leaves the screen's rate to the phone.

#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <android/native_window.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "blackbox.h"
#include "renderer/surface_lifecycle.h"

REXCVAR_DEFINE_BOOL(wwe2k14_android_frame_rate, true, "wwe2k14",
                    "Session 73 (Android): tell the phone the game is a fixed 60 fps source, so a 90 / 120 Hz screen "
                    "runs at 60 or 120 Hz (src/android_frame_rate.h)");

namespace wwe2k14::android_frame_rate {

class FrameRate {
 public:
  // Once per guest frame (frame_stats.h); looks at the window every 30 frames.
  void OnFrame(uint64_t frame) {
    if (off_ || frame % 30) return;
    if (!loaded_) {
      loaded_ = true;
      if (!REXCVAR_GET(wwe2k14_android_frame_rate) || !Load()) {
        off_ = true;
        return;
      }
    }
    SDL_Window* sdl_window = FirstWindow();
    if (!sdl_window) return;
    auto* window = static_cast<ANativeWindow*>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(sdl_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
    const auto now = std::chrono::steady_clock::now();
    const float want_hz = WantedHz();
    const uint32_t surface_epoch = rdr::WindowEvents().epoch();  // lane 235: a new window at the old address
    if (window && (window != set_for_ || want_hz != declared_hz_ || surface_epoch != set_epoch_)) {
      set_epoch_ = surface_epoch;
      set_for_ = window;
      declared_hz_ = want_hz;
      const float before = ScreenHz(sdl_window);
      // 1 = ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 1 = ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS (a
      // non-seamless switch, once at the start, rather than staying at a rate 60 does not divide).
      const int result = with_strategy_ ? with_strategy_(window, want_hz, 1, 1) : plain_(window, want_hz, 1);
      REXLOG_INFO("[wwe2k14][frame_rate] asked for a fixed {:.0f} fps source (result {}); the screen was {:.2f} Hz",
                  want_hz, result, before);
      blackbox::Note("screen %.2f Hz, asked for %.0f fps (result %d)", double(before), double(want_hz), result);
      check_at_ = now + std::chrono::seconds(3);
      checked_ = false;
    }
    if (!checked_ && set_for_ && now >= check_at_) {
      checked_ = true;
      REXLOG_INFO("[wwe2k14][frame_rate] the screen now runs at {:.2f} Hz", ScreenHz(sdl_window));
    }
  }

 private:
  // Session 95: 120 when the setting runs the game's step at 120 (--wwe2k14_step_hz=120, or the decoupled 120: gpu_d3d_decouple bit 8
  // with the word 0, as gfx_menu.h reads FRAME RATE 2), else 60.
  static float WantedHz() {
    auto flag_int = [](const char* name) {
      const std::string v = rex::cvar::GetFlagByName(name);
      return v.empty() ? 0 : std::atoi(v.c_str());
    };
    if (flag_int("wwe2k14_step_hz") == 120) return 120.0f;
    if (flag_int("wwe2k14_rdr_display_hz") == 120) return 120.0f;  // S100: the new renderer's decoupled 120 (the game's step stays 60)
    if ((flag_int("gpu_d3d_decouple") & 8) && flag_int("gpu_d3d_decouple_word") == 0) return 120.0f;
    return 60.0f;
  }

  using WithStrategyFn = int32_t (*)(ANativeWindow*, float, int8_t, int8_t);
  using PlainFn = int32_t (*)(ANativeWindow*, float, int8_t);

  bool Load() {
    void* lib = dlopen("libnativewindow.so", RTLD_NOW);
    if (!lib) lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return false;
    with_strategy_ = reinterpret_cast<WithStrategyFn>(dlsym(lib, "ANativeWindow_setFrameRateWithChangeStrategy"));
    plain_ = reinterpret_cast<PlainFn>(dlsym(lib, "ANativeWindow_setFrameRate"));
    if (!with_strategy_ && !plain_) {
      REXLOG_INFO("[wwe2k14][frame_rate] ANativeWindow_setFrameRate not available (Android before 11): off");
      return false;
    }
    return true;
  }

  static SDL_Window* FirstWindow() {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* first = (windows && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
    return first;
  }

  static float ScreenHz(SDL_Window* window) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return mode ? mode->refresh_rate : 0.0f;
  }

  bool loaded_ = false, off_ = false, checked_ = true;
  WithStrategyFn with_strategy_ = nullptr;
  PlainFn plain_ = nullptr;
  ANativeWindow* set_for_ = nullptr;
  uint32_t set_epoch_ = 0;
  float declared_hz_ = 0.0f;
  std::chrono::steady_clock::time_point check_at_{};
};

inline FrameRate g_frame_rate;

}  // namespace wwe2k14::android_frame_rate

#endif  // __ANDROID__
