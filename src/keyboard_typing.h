// wwe2k14 - typing on the PC keyboard into the game's own on-screen keyboard (Session 55 part 3).
//
// Jarrod (2026-10-23): "a new feature I'd like is for allowing for keyboard typing when text input fields come up
// (create a story for example), visually I think it would be super fun if the key you hit was briefly selected as you
// press it to type, giving it the red fade". --wwe2k14_keyboard_typing (default on since it was checked, run 3727:
// real keys with the keyboard-as-pad mapping on typed "Wasd Typed 1-9=[]", Enter accepted, Escape cancelled, the arrows
// moved the text cursor, the pad still worked).
//
// The game's keyboard (found live, run 3723: CREATIONS > CHAMPIONSHIP > ACCEPT > CHAMPIONSHIP NAME) is a grid widget:
//   - its vtable is 0x82048B70; slot 2 (0x82048B78) is its per-frame update sub_8292F3B8(this), which copies the
//     cursor (row this+20, column this+36) to this+224 / this+220 and runs the shared grid input sub_823BC8B0 (the
//     cursor moves in sub_823BC968; memory scanner + watch, run 3723). Three vtables share that update: only
//     0x82048B70 is taken as the keyboard.
//   - rows 0-3 hold "1234567890-=", "qwertyuiop[]" from column 1 and "asdfghjkl;'", "zxcvbnm,./|" from column 2
//     (column 0 is the SHIFT / SPACE / LB column; row 2 column 1 is the text cursor's left arrow); with SHIFT on:
//     "!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"", "ZXCVBNM<>?`". Rows 0 and 1 from the cursor ('7' = column 7,
//     'u' = row 1 column 7, run 3723); rows 2 and 3 from typing every key (run 3725).
//   - pad keys: A types the key under the cursor (the game's red highlight), LS = SHIFT (a toggle), Y = SPACE,
//     X = BACK (delete), START = ENTER, B = CANCEL, LB / RB move the text cursor.
//   - the real cursor is the keyboard model's, K = [[this+260]+8]: row byte K+3456, column byte K+3457. The widget's
//     row / column are its copy: after a move the keyboard's "cursor moved" sub_82933758(this) plays the cursor sound,
//     sets the model's cursor from them (sub_82933218(K, row, col)) and copies it back. Writing the copy alone typed the
//     key the cursor was on (run 3724).
// A typed character sets the widget's row / column to its key and runs that "cursor moved" (so the game moves its own
// cursor and highlight, as for a d-pad press), then presses A for the player (the game draws its selected key and the
// press itself); SHIFT is pressed first when the key's case
// differs from the screen's. Backspace = X, Enter = START, Escape = B, Space = Y, Left / Right = LB / RB. While the
// keyboard is up these keys go to it only: the keyboard-as-pad mapping (mnk_mode, z-order 0) and the binds never see
// them (this listener sits above them and marks them handled). Function keys and keys with Ctrl / Alt pass through.
// The character comes from SDL's text input (on while the keyboard is up), so non-US layouts type what their keys show
// (a character the game's grid has not got is ignored). SDL gives keys only to the window with the keyboard focus.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "input_script.h"  // g_typing_pad_filter

REXCVAR_DEFINE_BOOL(wwe2k14_keyboard_typing, true, "wwe2k14",
                    "Session 55: type on the PC keyboard into the game's on-screen keyboard (name fields: Create a "
                    "Story, championships, ...): each key typed moves the on-screen cursor to that key and presses it "
                    "(the game's own red highlight); Backspace / Enter / Escape / Space / Left / Right = X / START / B / "
                    "Y / LB / RB. While the keyboard is up those keys do not reach the keyboard-as-pad mapping");
REXCVAR_DEFINE_STRING(wwe2k14_keyboard_typing_test, "", "wwe2k14",
                      "Test (Session 55, scripts / live: `cvar wwe2k14_keyboard_typing_test TEXT`): types TEXT as if "
                      "typed on the PC keyboard (needs --wwe2k14_keyboard_typing); <BS> = Backspace, <ENTER> = Enter, "
                      "<ESC> = Escape, _ = a space")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace wwe2k14::typing {

constexpr uint32_t kKeyboardVtable = 0x82048B70;  // the keyboard grid widget's class (run 3723)
constexpr uint32_t kRowOffset = 20, kColumnOffset = 36;
constexpr uint16_t kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000, kPadStart = 0x0010, kPadLs = 0x0040,
                   kPadLb = 0x0100, kPadRb = 0x0200;
constexpr int kHoldFrames = 2, kGapFrames = 2;  // a press = held 2 widget frames, then 2 released (one clean edge)
constexpr size_t kMaxQueue = 64;

constexpr const char* kRows[2][4] = {{"1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./|"},
                                     {"!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"", "ZXCVBNM<>?`"}};
// The column of each row's first key (run 3725: row 2 column 1 is the text cursor's left arrow; row 3 column 1 lands
// on 'z' too, the model clamps it).
constexpr int kFirstColumn[4] = {1, 1, 2, 2};

// One thing typed: a character of the grid (key != 0) or a pad button of the screen's own (Y space, X back, ...).
struct Action {
  char key = 0;
  uint16_t button = 0;
};

struct State {
  std::mutex mutex;
  std::deque<Action> queue;           // UI thread -> game thread
  std::atomic<uint32_t> widget{0};    // the keyboard widget seen last (0 = none)
  std::atomic<int64_t> seen_ms{0};    // when its update last ran (steady clock)
  std::atomic<uint16_t> buttons{0};   // what the pad filter adds this frame (game thread)
  // Game thread only (the widget's update):
  uint32_t last_widget = 0;
  bool shift = false;  // the screen's SHIFT, tracked: off when a keyboard opens; our LS presses and the player's toggle it
  int phase_frames = 0;
  uint16_t phase_button = 0;
  bool releasing = false;
  Action pending_after_shift;
  bool have_pending = false;
  uint16_t prev_real = 0;
  std::atomic<uint64_t> typed{0}, ignored{0};
  std::bitset<256> consumed;  // UI thread: keys whose down went to the keyboard (their up goes there too)
  rex::ui::Window* window = nullptr;
  std::atomic<bool> text_input{false};  // SDL text input on (the typed characters arrive as OnKeyChar)
};

inline State& S() {
  static State s;
  return s;
}

inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The game's keyboard is up while its update ran in the last 250 ms (UI thread).
inline bool Active() { return S().widget.load() && NowMs() - S().seen_ms.load() < 250; }

inline bool Find(char c, int& row, int& column, bool& shifted) {
  for (int s = 0; s < 2; ++s)
    for (int r = 0; r < 4; ++r)
      if (const char* p = std::strchr(kRows[s][r], c); p && c) {
        row = r, column = int(p - kRows[s][r]) + kFirstColumn[r], shifted = s == 1;
        return true;
      }
  return false;
}

inline void Push(Action a) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (s.queue.size() < kMaxQueue) s.queue.push_back(a);
}

// SDL text input on / off (any thread: the window call itself goes through the UI thread's queue).
inline void SetTextInput(bool on) {
  State& s = S();
  if (!s.window || s.text_input.exchange(on) == on) return;
  rex::ui::Window* window = s.window;
  window->app_context().CallInUIThreadDeferred([window, on] { window->SetTextInputActive(on); });
}

// Game thread, once per frame of the keyboard widget (before its own update runs). move_cursor = the game's own "move to
// the widget's row / column" (sub_82933758: cursor sound, the keyboard model's cursor, the widget's copy).
inline void OnWidgetFrame(uint32_t self, const std::function<void()>& move_cursor) {
  State& s = S();
  if (self != s.last_widget || NowMs() - s.seen_ms.load() > 500) {  // a keyboard opened: SHIFT starts off (run 3723)
    s.last_widget = self;
    s.shift = false;
    s.phase_frames = 0;
    s.releasing = false;
    s.have_pending = false;
    s.buttons = 0;
    std::lock_guard lock(s.mutex);
    s.queue.clear();
    REXLOG_INFO("[wwe2k14][typing] keyboard up (widget {:08X}): type on the PC keyboard ({} typed, {} not on the "
                "grid so far)", self, s.typed.load(), s.ignored.load());
    SetTextInput(true);
  }
  s.widget = self;
  s.seen_ms = NowMs();
  if (s.phase_frames > 0) {  // a press in progress: held, then released
    if (--s.phase_frames == 0) {
      if (!s.releasing) {
        s.releasing = true;
        s.phase_frames = kGapFrames;
        s.buttons = 0;
      } else {
        s.releasing = false;
      }
    }
    if (s.phase_frames > 0) return;
  }
  Action a;
  if (s.have_pending) {
    a = s.pending_after_shift;
    s.have_pending = false;
  } else {
    std::lock_guard lock(s.mutex);
    if (s.queue.empty()) return;
    a = s.queue.front();
    s.queue.pop_front();
  }
  uint16_t button = a.button;
  if (a.key) {
    int row = 0, column = 0;
    bool shifted = false;
    if (!Find(a.key, row, column, shifted)) {
      ++s.ignored;
      return;
    }
    if (shifted != s.shift) {  // SHIFT first, then this key
      s.pending_after_shift = a;
      s.have_pending = true;
      s.shift = shifted;
      button = kPadLs;
    } else {
      rex::memory::store_and_swap<uint32_t>(
          REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(self + kRowOffset), uint32_t(row));
      rex::memory::store_and_swap<uint32_t>(
          REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(self + kColumnOffset), uint32_t(column));
      move_cursor();  // (the widget's row / column alone are only its copy: run 3724 typed the old key)
      button = kPadA;
      ++s.typed;
    }
  }
  s.phase_button = button;
  s.buttons = button;
  s.phase_frames = kHoldFrames;
}

// Game thread (XamInputGetState, player 1): our press goes on top of the real pad; the player's own LS toggles SHIFT.
inline void PadFilter(rex::input::X_INPUT_STATE* state) {
  State& s = S();
  const uint16_t real = state->gamepad.buttons;
  if (s.widget && (real & kPadLs) && !(s.prev_real & kPadLs)) s.shift = !s.shift;
  s.prev_real = real;
  if (s.text_input && NowMs() - s.seen_ms.load() > 300) SetTextInput(false);  // the keyboard closed
  // (Only while the widget's update still runs: ENTER closes the keyboard, and START must not stay held on the screen
  // after it.)
  if (const uint16_t add = s.buttons.load(); add && NowMs() - s.seen_ms.load() < 50) {
    state->gamepad.buttons = uint16_t(real | add);
  }
}

// UI thread: keys while the game's keyboard is up.
// Keys whose character the typed text brings (OnKeyChar): letters, digits, the punctuation keys, the number pad.
inline bool CharacterKey(uint16_t vk) {
  return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) ||
         (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102;
}

// UI thread: keys while the game's keyboard is up. The characters come from SDL's text input (OnKeyChar: the player's
// own layout, dead keys included; the key codes the SDK gives are US key positions), the other keys from OnKeyDown.
class Listener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const uint16_t vk = static_cast<uint16_t>(e.virtual_key());
    if (!Active()) {
      SetTextInput(false);
      return;
    }
    if (vk >= 256 || e.is_ctrl_pressed() || e.is_alt_pressed() || e.is_super_pressed()) return;
    Action a;
    switch (vk) {
      case VK_BACK: a.button = kPadX; break;
      case VK_RETURN: a.button = kPadStart; break;
      case VK_ESCAPE: a.button = kPadB; break;
      case VK_SPACE: a.button = kPadY; break;
      case VK_LEFT: a.button = kPadLb; break;
      case VK_RIGHT: a.button = kPadRb; break;
      default:
        if (!CharacterKey(vk)) return;  // function keys (our tools), arrows up / down, ... pass
        SetTextInput(true);              // (normally on since the keyboard opened)
        S().consumed.set(vk);            // the character itself comes as OnKeyChar
        e.set_handled(true);
        return;
    }
    S().consumed.set(vk);
    Push(a);
    e.set_handled(true);
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    const uint16_t vk = static_cast<uint16_t>(e.virtual_key());
    if (vk < 256 && S().consumed.test(vk)) {
      S().consumed.reset(vk);
      e.set_handled(true);
    }
  }
  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (!Active()) return;
    const uint32_t c = static_cast<uint32_t>(e.virtual_key());  // the code point (window_sdl.cpp)
    if (c > 0x20 && c < 0x7F) Push(Action{char(c), 0});           // (space: VK_SPACE above)
    else if (c != 0x20) ++S().ignored;
    e.set_handled(true);
  }
};

inline Listener& L() {
  static Listener l;
  return l;
}

// From ReXApp::OnCreateDialogs (UI thread). Returns at once unless --wwe2k14_keyboard_typing=true.
inline void Install(rex::ui::Window* window) {
  if (!REXCVAR_GET(wwe2k14_keyboard_typing) || !window) return;
  window->AddInputListener(&L(), 200);  // above CineCam (100), the ImGui drawer (64) and the keyboard-as-pad driver (0)
  g_typing_pad_filter = PadFilter;
  S().window = window;
  rex::cvar::RegisterChangeCallback("wwe2k14_keyboard_typing_test", [](std::string_view, std::string_view value) {
    const std::string text(value);
    size_t typed = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      Action a;
      if (text.compare(i, 4, "<BS>") == 0) a.button = kPadX, i += 3;
      else if (text.compare(i, 7, "<ENTER>") == 0) a.button = kPadStart, i += 6;
      else if (text.compare(i, 5, "<ESC>") == 0) a.button = kPadB, i += 4;
      else if (text[i] == '_') a.button = kPadY;
      else a.key = text[i];
      Push(a);
      ++typed;
    }
    REXLOG_INFO("[wwe2k14][typing] test: {} keys queued from '{}' (keyboard up: {})", typed, text, Active());
  });
  REXLOG_INFO("[wwe2k14][typing] installed: the PC keyboard types into the game's on-screen keyboard");
}

}  // namespace wwe2k14::typing

REX_EXTERN(__imp__sub_8292F3B8);
REX_EXTERN(sub_82933758);  // the keyboard's "cursor moved": model cursor = widget row / column (K+3456 / K+3457)
// The keyboard grid widget's per-frame update (see the header): our step runs first, so a cursor we set is the one
// its input sees.
REX_HOOK_RAW(sub_8292F3B8) {
  if (REXCVAR_GET(wwe2k14_keyboard_typing)) {
    const uint32_t self = ctx.r3.u32;
    if (self && rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(self)) ==
                    wwe2k14::typing::kKeyboardVtable) {
      wwe2k14::typing::OnWidgetFrame(self, [&] {
        const PPCContext saved = ctx;
        ctx.lr = 0;
        ctx.r3.u64 = self;
        sub_82933758(ctx, base);
        ctx = saved;
      });
    }
  }
  __imp__sub_8292F3B8(ctx, base);
}
