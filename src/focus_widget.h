#pragma once
// wwe2k14 - Session 91 step 6 (the game atlas; Jarrod: "go for that structural fix"): which input widget acted, and
// where its cursor is, on ANY menu screen - so the navigator (tools/atlas/nav.py game_where) reports "row r of n,
// column c of m" without a per-screen memory hunt.
// Session 108 lane MENU: the record is now ROUTE STATE too: the built-in states menu_ready / menu_page / menu_row /
// menu_rows / menu_col / menu_cols / menu_count (src/menu_focus.h holds the pick rule and the values; src/game_state.h
// the built-in table; docs/research/s108/MENU_plan.md). nav.py reads them with log_state instead of parsing the dump.
//
// Every menu cursor found goes through one shared input object (the "mixin", base vtable 0x8200F5E8; 415 vtables share
// its update sub_823BD358 and its dispatch sub_823BC8C0; the SmartMenu at SM+48 and each UI list widget's [list+924]
// are mixins too): out/harness/atlas/readers/focus_widget.md (Opus read, listings cited there). sub_823BD358 calls
// the dispatch once per tick only after its gates pass (the global block, +216 == 2 = active, focus level +212 >=
// [0x836F3AA4]), so "reached the dispatch this frame" = has the input now. Fields (mixin M = r3): row +20 (count +24),
// column +36 (count +40), moved flags +80 (1 up, 2 down, 4 left, 8 right; cleared every tick), pads it listens to +8.
// Counts 0 = the owner keeps its own cursor (SmartMenu SM+352, UI list [list+88]: menu_focus.h follows those).
//
// --wwe2k14_focus_dump: `cvar wwe2k14_focus_dump N` (a new N each time) logs the mixins that reached the dispatch in
// the last two guest frames: [wwe2k14][focus] lines (pointer, vtable, row / count, column / count, moved, pads) and
// the pick (the diagnosis of "ambiguous").
// Recording is always on (a few mixins a frame: a pointer, 9 words); pointers are only ever logged, never followed.

#include <cstdint>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "frame_stats.h"  // g_guest_frame
#include "game_state.h"   // g_host_state: where the menu_* states read from
#include "menu_focus.h"

REXCVAR_DEFINE_INT64(wwe2k14_focus_dump, 0, "wwe2k14",
                     "Session 91: set to a new value to log the input widgets that took the input in the last two "
                     "guest frames (src/focus_widget.h; the navigator's game_where)");

namespace wwe2k14::focus {

inline std::mutex g_mutex;  // UI lists may tick on another thread than the task runner's mixins (focus_widget.md, open)
// The recorded mixins (this and the previous guest frame), their change stamps and the UI list notes. UI list widgets
// (vtable 0x820660A8): their input mixin is a standalone one at [list+924] (made by sub_823BE5F8, stored at
// 0x82B48334) and the list keeps the cursor itself: [list+88] = the item, [list+612] = the count
// (docs/systems/menus.md "the UI system's list widget"). The list's input step sub_82B49A70 notes list -> mixin.
inline menu_focus::Tracker g_tracker;

inline void NoteList(uint32_t list, uint64_t frame);  // below (called from src/mouse_menu.h's sub_82B49A70 hook)
inline int64_t g_dump_seen = 0;

inline uint32_t R32(uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a));
}

inline void Record(uint32_t m, uint64_t frame) {
  g_tracker.Note(m, frame, [](uint32_t a) { return R32(a); });
}

inline void NoteList(uint32_t list, uint64_t frame) {
  if (!list) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_tracker.NoteList(list, R32(list + 924), frame);
}

inline void DumpIfAsked(uint64_t frame) {
  const int64_t ask = REXCVAR_GET(wwe2k14_focus_dump);
  if (ask == g_dump_seen) return;
  g_dump_seen = ask;
  std::string s;
  size_t n = 0;
  for (size_t i = 0; i < g_tracker.size(); ++i) {
    const menu_focus::MixinRec& e = g_tracker.at(i);
    if (e.frame + 1 < frame) continue;
    ++n;
    s += fmt::format(" | m={:08X} vt={:08X} row={}/{} col={}/{} moved={} pads={:X} f={} chg={}", e.m, e.vtable, e.row,
                     e.rows, e.col, e.cols, e.moved, e.pads, e.frame, e.change_frame);
    if (e.owner_kind == menu_focus::Owner::kUiList)
      s += fmt::format(" list={:08X} item={}/{}", e.owner, int32_t(R32(e.owner + 88)), int32_t(R32(e.owner + 612)));
    else if (e.owner_kind == menu_focus::Owner::kSmartMenu)
      s += fmt::format(" smartmenu={:08X} cursor={}", e.owner, int32_t(R32(e.owner + 352)));
  }
  const menu_focus::Pick p = g_tracker.Choose(frame);
  REXLOG_INFO("[wwe2k14][focus] frame {}: {} widgets took the input{}", frame, n, s);
  REXLOG_INFO("[wwe2k14][focus] pick: {}{}", p.rec ? fmt::format("m={:08X}", p.rec->m) : std::string("none"),
              p.tied > 1 ? fmt::format(" ambiguous {} (tie on the change stamp)", p.tied) : std::string());
}

// The built-in route states menu_* (GuestState::host_field, src/game_state.h): read under the mutex the hook records
// under; the cursor words are read live, so on the SmartMenu menu_row is the same word `cursor` reads. The pointers
// were recorded up to a frame ago (a page replaced at S+80 can be freed since), so the words go through the checked
// read every chain uses (ReadGuestBytes): a word that cannot be read makes the state unreadable, never a host fault.
inline bool ReadMenu(uint32_t field, uint32_t* out) {
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mutex);
  bool words_ok = true;
  const auto rd = [&words_ok](uint32_t a) {
    uint32_t v = 0;
    if (!wwe2k14::ReadGuestBytes(a, 4, &v)) words_ok = false;
    return v;
  };
  const bool got = g_tracker.Read(menu_focus::Field(field), frame, rd, out);
  return got && words_ok;
}
inline const bool g_menu_states_registered = (wwe2k14::g_host_state = &ReadMenu, true);

}  // namespace wwe2k14::focus

REX_EXTERN(__imp__sub_823BC8C0);
// The shared input mixin's dispatch (r3 = the mixin), called by its update sub_823BD358 at 0x823BD760 once the gates
// passed. The cursor fields are read after the move (the dispatch runs the move sub_823BC968).
REX_HOOK_RAW(sub_823BC8C0) {
  const uint32_t m = ctx.r3.u32;
  __imp__sub_823BC8C0(ctx, base);
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(wwe2k14::focus::g_mutex);
  wwe2k14::focus::Record(m, frame);
  wwe2k14::focus::DumpIfAsked(frame);
}
