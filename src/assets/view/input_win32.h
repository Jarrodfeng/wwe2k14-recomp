// Session 98 (freecam): Win32 input for the viewer's free camera: held keys by polling, and the first XInput pad (loaded at run time, so
// the build needs no extra library). freecam.h itself holds no Win32.
#pragma once
#include <windows.h>

#include <cmath>

#include "freecam.h"

namespace view {

// Held keys, polled (a key-up that a UI popup swallowed cannot leave a key stuck). Only while `focused`.
inline void PollFreeKeys(FreeKeys& k, bool focused) {
  auto down = [&](int vk) { return focused && (GetAsyncKeyState(vk) & 0x8000) != 0; };
  k.fwd = down('W'), k.back = down('S'), k.left = down('A'), k.right = down('D');
  k.up = down('E'), k.down = down('Q');
  k.fast = down(VK_SHIFT), k.fine = down(VK_CONTROL);
  k.roll_left = down('Z'), k.roll_right = down('X'), k.roll_reset = down('R');
}

struct XPadState {  // the layout of XINPUT_STATE (xinput.h), so no SDK header is needed
  DWORD packet;
  WORD buttons;
  BYTE lt, rt;
  SHORT lx, ly, rx, ry;
};

class Pad {
 public:
  // Fills the pad_* fields of `c` (all zero when no pad / not focused). Disconnected pads are looked for once a second
  // (XInputGetState on an empty slot is slow).
  void Poll(FreeCamCtl& c, bool focused) {
    c.pad_move_x = c.pad_move_y = c.pad_look_x = c.pad_look_y = c.pad_up = c.pad_zoom = 0;
    if (!focused || !Load()) return;
    const DWORD now = GetTickCount();
    if (user_ < 0) {
      if (now - last_scan_ < 1000) return;
      last_scan_ = now;
      XPadState s0;  // one empty slot per second (each failed call is slow): a pad is found within 4 s
      const int slot = scan_slot_;
      scan_slot_ = (scan_slot_ + 1) % 4;
      if (get_(slot, &s0) == ERROR_SUCCESS) user_ = slot;
      if (user_ < 0) return;
    }
    XPadState s;
    if (get_(user_, &s) != ERROR_SUCCESS) {
      user_ = -1;
      return;
    }
    c.pad_move_x = Stick(s.lx);
    c.pad_move_y = Stick(s.ly);
    c.pad_look_x = Stick(s.rx);
    c.pad_look_y = Stick(s.ry);
    c.pad_up = Trigger(s.rt) - Trigger(s.lt);
    c.pad_zoom = float((s.buttons & 0x0200) != 0) - float((s.buttons & 0x0100) != 0);  // RB in, LB out
  }

 private:
  using GetStateFn = DWORD(WINAPI*)(DWORD, XPadState*);
  bool Load() {
    if (tried_) return get_ != nullptr;
    tried_ = true;
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryA(dll)) {
        get_ = (GetStateFn)(void*)GetProcAddress(m, "XInputGetState");
        if (get_) break;
      }
    }
    return get_ != nullptr;
  }
  static float Stick(SHORT v) {  // dead zone 0.24 of full scale, then rescaled to 0..1
    const float x = float(v) / 32768.0f, a = std::fabs(x);
    if (a < 0.24f) return 0;
    return (x < 0 ? -1.0f : 1.0f) * std::min(1.0f, (a - 0.24f) / 0.76f);
  }
  static float Trigger(BYTE v) {
    const float x = float(v) / 255.0f;
    return x < 0.12f ? 0.0f : (x - 0.12f) / 0.88f;
  }
  bool tried_ = false;
  GetStateFn get_ = nullptr;
  int user_ = -1, scan_slot_ = 0;
  DWORD last_scan_ = 0;
};

}  // namespace view
