#pragma once
// wwe2k14 - Session 91 step 6 (the game atlas; Jarrod: "go for that structural fix"): which input widget acted, and
// where its cursor is, on ANY menu screen - so the navigator (tools/atlas/nav.py game_where) reports "row r of n,
// column c of m" without a per-screen memory hunt.
//
// Every menu cursor found goes through one shared input object (the "mixin", base vtable 0x8200F5E8; 415 vtables share
// its update sub_823BD358 and its dispatch sub_823BC8C0; the SmartMenu at SM+48 and each UI list widget's [list+924]
// are mixins too): out/harness/atlas/readers/focus_widget.md (Opus read, listings cited there). sub_823BD358 calls
// the dispatch once per tick only after its gates pass (the global block, +216 == 2 = active, focus level +212 >=
// [0x836F3AA4]), so "reached the dispatch this frame" = has the input now. Fields (mixin M = r3): row +20 (count +24),
// column +36 (count +40), moved flags +80 (1 up, 2 down, 4 left, 8 right; cleared every tick), pads it listens to +8.
// Counts 0 = the owner keeps its own cursor (SmartMenu SM+352, UI list [list+88]: the navigator reads those).
//
// --wwe2k14_focus_dump: `cvar wwe2k14_focus_dump N` (a new N each time) logs the mixins that reached the dispatch in
// the last two guest frames: [wwe2k14][focus] lines (pointer, vtable, row / count, column / count, moved, pads).
// Recording is always on (a few mixins a frame: a pointer, 9 words); pointers are only ever logged, never followed.

#include <array>
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

REXCVAR_DEFINE_INT64(wwe2k14_focus_dump, 0, "wwe2k14",
                     "Session 91: set to a new value to log the input widgets that took the input in the last two "
                     "guest frames (src/focus_widget.h; the navigator's game_where)");

namespace wwe2k14::focus {

struct Entry {
  uint32_t m = 0, vtable = 0, pads = 0;
  int32_t row = 0, rows = 0, col = 0, cols = 0;
  uint32_t moved = 0;
  uint64_t frame = 0;
};

inline std::mutex g_mutex;  // UI lists may tick on another thread than the task runner's mixins (focus_widget.md, open)
inline std::array<Entry, 32> g_entries{};  // the dispatches of the current and the previous guest frame
inline size_t g_count = 0;
// UI list widgets (vtable 0x820660A8): their input mixin is a standalone one at [list+924] (made by sub_823BE5F8,
// stored at 0x82B48334) and the list keeps the cursor itself: [list+88] = the item, [list+612] = the count
// (docs/systems/menus.md "the UI system's list widget"). The list's input step sub_82B49A70 notes list -> mixin.
struct ListNote {
  uint32_t list = 0, mixin = 0;
  uint64_t frame = 0;
};
inline std::array<ListNote, 16> g_lists{};

inline void NoteList(uint32_t list, uint64_t frame);  // below (called from src/mouse_menu.h's sub_82B49A70 hook)
inline int64_t g_dump_seen = 0;

inline uint32_t R32(uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a));
}

inline void Record(uint32_t m, uint64_t frame) {
  // keep this frame's and the previous frame's entries; drop older ones
  size_t keep = 0;
  for (size_t i = 0; i < g_count; ++i)
    if (g_entries[i].frame + 1 >= frame && g_entries[i].m != m) g_entries[keep++] = g_entries[i];
  g_count = keep;
  if (g_count == g_entries.size()) return;
  Entry e;
  e.m = m, e.frame = frame, e.vtable = R32(m), e.pads = R32(m + 8);
  e.row = int32_t(R32(m + 20)), e.rows = int32_t(R32(m + 24)), e.col = int32_t(R32(m + 36)),
  e.cols = int32_t(R32(m + 40)), e.moved = R32(m + 80);
  g_entries[g_count++] = e;
}

inline void NoteList(uint32_t list, uint64_t frame) {
  if (!list) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint32_t mixin = R32(list + 924);
  ListNote* slot = &g_lists[0];
  for (ListNote& n : g_lists) {
    if (n.list == list) {
      slot = &n;
      break;
    }
    if (n.frame < slot->frame) slot = &n;  // else the oldest note
  }
  *slot = {list, mixin, frame};
}

inline void DumpIfAsked(uint64_t frame) {
  const int64_t ask = REXCVAR_GET(wwe2k14_focus_dump);
  if (ask == g_dump_seen) return;
  g_dump_seen = ask;
  std::string s;
  size_t n = 0;
  for (size_t i = 0; i < g_count; ++i) {
    const Entry& e = g_entries[i];
    if (e.frame + 1 < frame) continue;
    ++n;
    s += fmt::format(" | m={:08X} vt={:08X} row={}/{} col={}/{} moved={} pads={:X} f={}", e.m, e.vtable, e.row, e.rows,
                     e.col, e.cols, e.moved, e.pads, e.frame);
    for (const ListNote& n : g_lists) {  // a UI list noted this or last frame with this mixin: its own cursor
      if (n.mixin == e.m && n.frame + 1 >= frame && R32(n.list) == 0x820660A8u) {
        s += fmt::format(" list={:08X} item={}/{}", n.list, int32_t(R32(n.list + 88)), int32_t(R32(n.list + 612)));
        break;
      }
    }
  }
  REXLOG_INFO("[wwe2k14][focus] frame {}: {} widgets took the input{}", frame, n, s);
}

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
