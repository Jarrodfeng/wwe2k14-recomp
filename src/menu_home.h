// S110 HOMESTEP: the guest side of the route command `home` (the rules are src/menu_home_core.h; the presses are made by
// src/input_script.h `case Op::kHome`). TEST / TOOLING PATH ONLY. Reads guest memory on the game thread, writes nothing.
// `menu_ready` / `menu_page` come from the same built-in states the script's `wait_state menu_ready` reads (game_state.h MakeHostState
// -> ReadGuestState -> the focus reader src/focus_widget.h registers). Every address is named in menu_home_core.h / menu_goto_core.h
// with the note claim it came from.
#pragma once

#include <rex/logging.h>

#include <cstdint>
#include <string>

#include "game_state.h"
#include "menu_goto_core.h"
#include "menu_home_core.h"

namespace wwe2k14::menu_home {

// scenario.h cannot be included here (it needs the event bus declared first, and input_script.h comes before it): the guest readers
// are registered by src/menu_goto.h, which already includes scenario.h (the same way src/focus_widget.h registers g_host_state).
struct Env {
  uint32_t (*u32)(uint32_t addr) = nullptr;           // scenario::U32
  uint32_t (*sequence)() = nullptr;                   // scenario::SequenceId
  const uint8_t* (*bytes)(uint32_t addr) = nullptr;   // scenario::Guest
};
inline Env g_env;
constexpr uint32_t kSmartMenuWord = 0x8392945C;  // scenario.h kSmartMenuWord

inline uint32_t Rd(uint32_t a) { return (g_env.u32 && a >= 0x10000 && a < 0xC0000000u) ? g_env.u32(a) : 0; }

inline bool ReadHost(const char* name, uint32_t* out) {
  GuestState st;
  return MakeHostState(name, st) && ReadGuestState(st, out);
}

// The box's own text for messages (MSGBOX claim 11 [C]: expanded at box+361). The encoding is not read [U]: printable ASCII only.
inline std::string BoxText(uint32_t box) {
  std::string s;
  const uint8_t* p = g_env.bytes ? g_env.bytes(box + kBoxChars) : nullptr;
  for (uint32_t i = 0; p && i < 80 && p[i] >= 0x20 && p[i] < 0x7F; ++i) s += char(p[i]);
  return s;
}

inline View ReadView(uint64_t frame) {
  namespace mg = wwe2k14::menu_goto;
  View v;
  v.frame = frame;
  v.sm = Rd(kSmartMenuWord);
  v.seq = g_env.sequence ? g_env.sequence() : 0;
  const bool front = InFrontEnd(v);
  const uint32_t holder = front ? Rd(mg::kHolderPtr) : 0;
  v.table = holder != 0;  // menu_goto::Tick reads the whole table; `home` needs only that the holder exists (its state follows)
  v.holder_state = holder ? Rd(holder + mg::kHolderState) : 0;
  v.lock56 = front ? Rd(Rd(v.sm + mg::kSmLockPtr) + 56) : 0;
  v.list_state = front ? Rd(v.sm + mg::kSmListState) : 0;
  v.armed = front && Rd(v.sm + mg::kMbArmed) != 0;
  v.menu = front ? Rd(v.sm + mg::kSmMenu) : 0;
  v.ready_readable = ReadHost("menu_ready", &v.menu_ready);
  v.page_readable = ReadHost("menu_page", &v.menu_page);
  v.box_obj = Rd(kBoxPtr);
  if (v.box_obj) {
    v.box_state = Rd(v.box_obj + kBoxState);
    if (v.box_state >= kBoxOpening && v.box_state <= kBoxClosing) {  // text / kind / cursor are valid only while the box is not 0 / 5
      v.box_id = Rd(v.box_obj + kBoxText);
      v.box_kind = Rd(v.box_obj + kBoxKind);
      v.box_cursor = Rd(v.box_obj + kBoxCursor);
      v.box_text = BoxText(v.box_obj);
    }
  }
  return v;
}

// One frame of a `home` line. `first` = the line just began. The caller presses what the Decision says and logs / fails it.
inline Decision Step(uint64_t frame, bool first) {
  static Home home;
  if (!g_env.u32 || !g_env.sequence) {  // nobody registered the readers: loud, never a guess
    Decision d;
    d.act = Act::kFail;
    d.why = "the guest readers are not registered (src/menu_goto.h is not built in)";
    return d;
  }
  if (first) home.Begin(frame);
  return home.Decide(ReadView(frame));
}

}  // namespace wwe2k14::menu_home
