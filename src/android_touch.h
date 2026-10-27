#pragma once
// wwe2k14 - Session 71 (Android only, ANDROID_PLAN F1; Jarrod 2026-10-28: "Build now, test later"): the on-screen touch
// controls' pad. TouchOverlay.java draws the buttons over the game and reports the whole pad state on every touch
// change through nativeUpdate; here that state goes into an SDL virtual gamepad, so the SDK's SDL input driver sees an
// ordinary pad. The SDK marks virtual pads synthetic (SDK s71-android, sdl_input_driver.cpp): the touch pad joins guest
// user 0 next to a real controller and never takes player 1 away from one connected later.
//
// nativeSetActive(true / false) attaches / removes the pad (the overlay hides while a controller is connected in its
// automatic mode, or when the player switched it off). Button bits = SDL_GamepadButton numbers; sticks -1..1 (y down
// positive, as SDL), triggers 0..1. Untested on the phone at the time of writing (no phone in Session 71).

#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

#include <rex/logging.h>

namespace wwe2k14::android_touch {

inline std::mutex g_mutex;
inline SDL_JoystickID g_id = 0;
inline SDL_Joystick* g_joy = nullptr;
// GUIDE is declared too (never pressed): SDL numbers a virtual pad's buttons over the declared ones only, in this
// order (SDL_virtualjoystick.c, the virtual gamepad mapping), so a gap shifts every later button. Without GUIDE, b42's
// START reached the game as L3, LB as RB, RB as d-pad up ... (Jarrod, 2026-10-28: "Start button isn't working").
// With buttons 0-14 all declared, button number = SDL_GamepadButton.
inline constexpr uint32_t kButtons =
    (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) | (1u << SDL_GAMEPAD_BUTTON_WEST) |
    (1u << SDL_GAMEPAD_BUTTON_NORTH) | (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_GUIDE) |
    (1u << SDL_GAMEPAD_BUTTON_START) |
    (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) |
    (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) |
    (1u << SDL_GAMEPAD_BUTTON_DPAD_UP) | (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) | (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) |
    (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

// False while SDL's joysticks are not up yet (the overlay tries again a second later).
inline bool SetActiveLocked(bool on) {
  if (on == (g_joy != nullptr)) return true;
  if (on) {
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) return false;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = kButtons;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "WWE 2K14 touch controls";
    g_id = SDL_AttachVirtualJoystick(&desc);
    g_joy = g_id ? SDL_OpenJoystick(g_id) : nullptr;
    if (!g_joy) {
      REXLOG_WARN("[wwe2k14][touch] virtual pad failed: {}", SDL_GetError());
      if (g_id) SDL_DetachVirtualJoystick(g_id), g_id = 0;
      return false;
    }
    REXLOG_INFO("[wwe2k14][touch] touch controls on (virtual pad {})", g_id);
  } else {
    SDL_CloseJoystick(g_joy);
    SDL_DetachVirtualJoystick(g_id);
    g_joy = nullptr, g_id = 0;
    REXLOG_INFO("[wwe2k14][touch] touch controls off");
  }
  return true;
}

inline Sint16 Axis(float v) { return Sint16(std::clamp(v, -1.f, 1.f) * 32767.f); }

}  // namespace wwe2k14::android_touch

extern "C" JNIEXPORT jboolean JNICALL Java_io_github_jarrodfeng_wwe2k14_TouchOverlay_nativeSetActive(JNIEnv*, jclass,
                                                                                                   jboolean on) {
  std::lock_guard lock(wwe2k14::android_touch::g_mutex);
  return wwe2k14::android_touch::SetActiveLocked(on) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_io_github_jarrodfeng_wwe2k14_TouchOverlay_nativeUpdate(
    JNIEnv*, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat lt, jfloat rt) {
  using namespace wwe2k14::android_touch;
  std::lock_guard lock(g_mutex);
  if (!g_joy) return;
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
    if (kButtons & (1u << b)) SDL_SetJoystickVirtualButton(g_joy, b, (uint32_t(buttons) >> b) & 1);
  SDL_SetJoystickVirtualAxis(g_joy, SDL_GAMEPAD_AXIS_LEFTX, Axis(lx));
  SDL_SetJoystickVirtualAxis(g_joy, SDL_GAMEPAD_AXIS_LEFTY, Axis(ly));
  SDL_SetJoystickVirtualAxis(g_joy, SDL_GAMEPAD_AXIS_RIGHTX, Axis(rx));
  SDL_SetJoystickVirtualAxis(g_joy, SDL_GAMEPAD_AXIS_RIGHTY, Axis(ry));
  // Triggers: a virtual pad's trigger axis is read over the whole joystick range, -32768 released .. 32767 fully
  // pressed (SDL_gamepad.c sets the trigger's zero to the input minimum). 0 would be half pressed (b42 sent that).
  auto trigger = [](float t) { return Sint16(-32768 + int(std::clamp(t, 0.f, 1.f) * 65535.f)); };
  SDL_SetJoystickVirtualAxis(g_joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(lt));
  SDL_SetJoystickVirtualAxis(g_joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, trigger(rt));
}

#endif  // __ANDROID__
