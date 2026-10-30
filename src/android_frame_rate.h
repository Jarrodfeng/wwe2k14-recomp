#pragma once
// Session 73 (Jarrod's idea 8 of the third ten: smooth frame timing on 90 / 120 Hz phone screens). The game makes a
// frame on its own 60 Hz vblank clock (30 fps frames every other one), but nothing told Android that: a phone left at
// 90 Hz shows 60 fps frames for 1, 2, 1, 2 ... refreshes (11 / 22 ms, an even judder), and the screen may change its
// rate under the game (adaptive refresh). ANativeWindow_setFrameRateWithChangeStrategy (Android 12; Android 11's
// ANativeWindow_setFrameRate without the strategy) says "a fixed 60 fps source": SurfaceFlinger then runs the screen
// at a multiple of 60 (60 or 120 Hz) while the game is in front. Always 60: menus, matches and 60 fps entrances run at
// 60, and 30 fps frames divide 60 and 120 evenly too, so the screen never switches mid-game.
// Set once per window (Android makes a new ANativeWindow when the app comes back from the background) and logged with
// the screen's rate before and a few seconds after: "[wwe2k14][frame_rate]". Functions by dlsym (the APK's minimum is
// Android 9). --wwe2k14_android_frame_rate=false leaves the screen's rate to the phone.

#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <android/native_window.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "blackbox.h"

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
    if (window && window != set_for_) {
      set_for_ = window;
      const float before = ScreenHz(sdl_window);
      // 1 = ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 1 = ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS (a
      // non-seamless switch, once at the start, rather than staying at a rate 60 does not divide).
      const int result = with_strategy_ ? with_strategy_(window, 60.0f, 1, 1) : plain_(window, 60.0f, 1);
      REXLOG_INFO("[wwe2k14][frame_rate] asked for a fixed 60 fps source (result {}); the screen was {:.2f} Hz",
                  result, before);
      blackbox::Note("screen %.2f Hz, asked for 60 fps (result %d)", double(before), result);
      check_at_ = now + std::chrono::seconds(3);
      checked_ = false;
    }
    if (!checked_ && set_for_ && now >= check_at_) {
      checked_ = true;
      REXLOG_INFO("[wwe2k14][frame_rate] the screen now runs at {:.2f} Hz", ScreenHz(sdl_window));
    }
  }

 private:
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
  std::chrono::steady_clock::time_point check_at_{};
};

inline FrameRate g_frame_rate;

}  // namespace wwe2k14::android_frame_rate

#endif  // __ANDROID__
