// S110 LETTER (docs\research\s110\LETTERJUMP.md; the owner's idea, docs\IDEAS.md:352): in the exhibition SELECT SUPERSTAR screen the RIGHT TRIGGER moves
// the pressing player's cursor to the first character of the next letter present in the list he is looking at, the LEFT TRIGGER to the previous
// (wrapping), in every tab (the stock grid and the four list tabs). Additive and toggled: --wwe2k14_letter_jump (default ON in this lane's code; the
// player default is read through this ONE switch, LetterJumpOn()).
//
// Hooks: the select panel's LT / RT handlers sub_823BC468 / sub_823BC5D0 (r3 = the pressing panel's mixin, panel = mixin - 44; the stock handler is
// empty and is always called afterwards). The jump is the game's own "cursor to row N" pair, run on the game thread inside the input dispatch:
//   sub_8285A160(panel, N)   places the cursor (list tabs: the row words; grid: the id's cell)
//   sub_8285BD18(panel, 0)   what the game's own "cursor moved" does next: the participant id, the portrait request, the highlight change
// The rows, their order and the names come from the game (list [panel+8836], ids, the roster row, the text manager): nothing is kept here.
// The pure half (initials, which row) is src\letter_jump_core.h (unit: tools\letter_jump\letter_jump_unit.cpp).
// Logs: one line per jump and one line when the hook is first reached; nothing per frame.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "guest_call.h"
#include "letter_jump_core.h"
#include "seats.h"  // Plausible / Readable / P / Alloc (the guest memory helpers)

REXCVAR_DEFINE_BOOL(wwe2k14_letter_jump, true, "wwe2k14",
                    "S110 LETTER: in the exhibition SELECT SUPERSTAR screen the right / left trigger moves your cursor to the first character of the next / "
                    "previous letter present in the list you are looking at (every tab, wrapping). Off = the triggers do what they did. See src/letter_jump.h");

REX_EXTERN(sub_8285A160);  // cursor to row N (r3 = the panel, r4 = N)
REX_EXTERN(sub_8285BD18);  // the game's "cursor moved" (r3 = the panel, r4 = 0)
REX_EXTERN(sub_8293A1F8);  // grid: find an id's cell (r3 = [screen + 56792], r4 = id, r5 = &col, r6 = &row) -> non-zero when found
REX_EXTERN(sub_8282C298);  // grid: may the cell walk rest on this id (r3 = [screen + 56796], r4 = id): sub_8282C648 +33C, the walk's own test
REX_EXTERN(sub_828568B8);  // the panel's current list index (r3 = the panel): +64, or +37064 when sub_82856108 (what sub_8285BD18 reads)
REX_EXTERN(sub_82856108);  // r3 = the panel: 1 = state 4 in the grid, where the game's "cursor moved" reads +37064 (the list index)
REX_EXTERN(sub_827D92F0);  // a created superstar's record (r3 = id < 100); its name is inline at +204 (sub_822C0D90)
REX_EXTERN(sub_822BCFB8);  // is the character id available (r3 = id)
REX_EXTERN(sub_8282B270);  // tab filters (r3 = the screen, r4 = id): DIVAS
REX_EXTERN(sub_8282F710);  //   CUSTOM
REX_EXTERN(sub_8282F748);  //   DOWNLOADED
REX_EXTERN(sub_8282B2A8);  //   MANAGERS
REX_EXTERN(sub_822C0CA8);  // is the id in the DIVAS tab's exclusion set (r3 = id)
REX_EXTERN(sub_827DA788);  // the roster row of an id (0..99 the created-superstar records, else the 204-byte rows)
REX_EXTERN(sub_82256510);  // text getter (r3 = the text manager [0x836F3A50], r4 = the text id) -> the guest string
REX_EXTERN(__imp__sub_823BC468);
REX_EXTERN(__imp__sub_823BC5D0);

namespace wwe2k14::letter_jump {

constexpr uint32_t kMixinVtable = 0x82038500;  // [mixin] of the exhibition select panel
constexpr uint32_t kTextManagerPtr = 0x836F3A50;
constexpr int kMaxRows = 500;  // ids are < 500; the list holds 1000 words

// Panel words (panel = mixin - 44); LETTERJUMP.md claim 1 and the listings of sub_8285A160 / sub_828584F0 / sub_8285BD18.
constexpr uint32_t kScreen = 264;       // [panel + 264] = the screen (= mixin + 220)
constexpr uint32_t kState = 8556;       // 2..4 = picking
constexpr uint32_t kListShown = 8836;   // which of the 6 lists the panel shows
constexpr uint32_t kListCount = 12888;  // + 4004 L: the list's row count; the ids follow at + 4 + 4 i
constexpr uint32_t kListStride = 4004;
constexpr uint32_t kTab = 37068;        // 0 grid, 1 DIVAS, 2 CUSTOM, 3 DOWNLOADED, 4 MANAGERS
constexpr uint32_t kDivasMode = 288;    // tab 1's exclusion set is skipped when this is 2 (sub_828584F0 +2E0)
constexpr uint32_t kGuard = 48428;      // [screen + 48428] == 0 (sub_82828B10)
constexpr uint32_t kTableCell = 56792;  // [screen + 56792] = the grid's cell table
constexpr uint32_t kGridWalk = 56796;   // [screen + 56796] = the grid walk's object (sub_8282F550 -> sub_8282C648 r3)
constexpr uint32_t kGridRows = 5;       // the walk rests only on a character cell in rows 0..4 (sub_8282C648 +334; row 5 is the special row)
constexpr uint32_t kCawIds = 100;       // ids 0..99 are created superstars (sub_827DA788 / sub_822C0D90 split there)
constexpr uint32_t kCawName = 204;      // a created superstar's name, inline at its record +204 (sub_822C0D90 +60; docs\formats\save_data.md)

// The player default lives here (the owner decides it with the main session): one switch for every use.
inline bool LetterJumpOn() { return REXCVAR_GET(wwe2k14_letter_jump); }

// THE ONE COUPLING with the downloaded-tab pager (src\seat_bind.h, step 3 of the roster work, not written yet): before a target is computed the provider is
// asked; true = it handled the press (it may flip a page and queue a jump) and nothing more is done here. Null today.
inline std::function<bool(uint32_t panel, int dir, PPCContext&, uint8_t*)> g_page_provider;

inline std::atomic<bool> g_logged_first{false};
inline uint32_t g_scratch = 0;  // 16 guest bytes for sub_8293A1F8's two out words

inline bool Rd(uint8_t* base, uint32_t a, uint32_t& v) {
  if (!seats::Readable(a, 4)) return false;
  v = seats::U32(base, a);
  return true;
}

// The display name of an id, by the game's own split (sub_822C0D90): a created superstar (id < 100) carries it inline at its record +204; any other id's
// roster row has the text id at +6 (u16) -> the text manager's string (the no-report getter: a missing text answers 0, no fallback written). "" = none.
inline std::string NameOf(uint8_t* base, scene_director::Caller& call, uint32_t id, uint32_t mgr) {
  uint32_t s = 0;
  if (id < kCawIds) {
    const uint32_t rec = call(sub_827D92F0, {id});
    if (!seats::Plausible(rec)) return {};
    s = rec + kCawName;
  } else {
    const uint32_t row = call(sub_827DA788, {id});
    if (!seats::Plausible(row) || !seats::Readable(row, 8)) return {};
    const uint8_t* r = seats::P(base, row);
    const uint32_t text = (uint32_t(r[6]) << 8) | r[7];
    if (text == 0) return {};
    s = call(sub_82256510, {mgr, text});
  }
  if (!seats::Plausible(s)) return {};
  std::string out;
  for (uint32_t i = 0; i < 64; ++i) {
    if (!seats::Readable(s + i, 1)) break;
    const char c = char(*seats::P(base, s + i));
    if (!c) break;
    out.push_back(c);
  }
  return out;
}

// Could the panel rest on this row? The game's own tests, so a jump never lands where UP / DOWN / LEFT / RIGHT cannot:
//   grid: the id's cell (sub_8293A1F8, what sub_8285A160 places), in rows 0..4, and the cell walk's own test sub_8282C298 (sub_8282C648 +320..+33C;
//         the walk does not ask sub_822BCFB8);
//   list tabs: sub_8285A160's walk (sub_828584F0 with r4 = 1, +2C4..+354): availability, then the tab's filter.
inline bool Selectable(uint8_t* base, scene_director::Caller& call, uint32_t panel, uint32_t screen, uint32_t tab, uint32_t id) {
  if (id >= uint32_t(kMaxRows)) return false;
  if (tab == 0) {
    uint32_t table = 0, walker = 0, row = 0;
    if (!Rd(base, screen + kTableCell, table) || !table || !Rd(base, screen + kGridWalk, walker) || !walker || !g_scratch) return false;
    seats::PutBe32(seats::P(base, g_scratch), 0xFFFFFFFFu);
    seats::PutBe32(seats::P(base, g_scratch + 4), 0xFFFFFFFFu);
    if (call(sub_8293A1F8, {table, id, g_scratch, g_scratch + 4}) == 0) return false;
    if (!Rd(base, g_scratch + 4, row) || !(row < kGridRows)) return false;
    return call(sub_8282C298, {walker, id}) != 0;
  }
  if (call(sub_822BCFB8, {id}) == 0) return false;
  switch (tab) {
    case 1: {
      uint32_t mode = 0;
      if (!Rd(base, panel + kDivasMode, mode)) return false;
      if (mode != 2 && call(sub_822C0CA8, {id}) != 0) return false;
      return call(sub_8282B270, {screen, id}) == 1;
    }
    case 2: return call(sub_8282F710, {screen, id}) == 1;
    case 3: return call(sub_8282F748, {screen, id}) == 1;
    case 4: return call(sub_8282B2A8, {screen, id}) == 1;
    default: return false;
  }
}

// The state a panel must be in for the jump (and for the letter strip, src\letter_ui.h): the select panel's mixin, the guard-0 screen, picking (2..4), a
// known tab, and (grid) the state where the game's "cursor moved" reads the list index.
struct Gated {
  uint32_t panel = 0, screen = 0, state = 0, tab = 0;
};

inline bool GateOf(scene_director::Caller& call, uint8_t* base, uint32_t panel, Gated& g) {
  const uint32_t mixin = panel + 44;
  uint32_t vt = 0, screen = 0, state = 0, guard = 1, tab = 0;
  if (!seats::Plausible(mixin) || !Rd(base, mixin, vt) || vt != kMixinVtable) return false;  // not the select panel's mixin
  if (!Rd(base, mixin + 220, screen) || !seats::Plausible(screen) || !Rd(base, screen + kGuard, guard) || guard != 0) return false;
  if (!Rd(base, panel + kState, state) || state < 2 || state > 4) return false;
  if (!Rd(base, panel + kTab, tab) || tab > 4) return false;
  // The grid's "cursor moved" (sub_8285BD18) reads the list index +37064 only where sub_82856108 says so (state 4); in states 2 / 3 it reads +64, which
  // in the grid is a cell ROW, so the participant would become list[row]: no jump there (the guard-0 screen enters the grid in state 4, sub_8285F610 +98).
  if (tab == 0 && call(sub_82856108, {panel}) == 0) return false;
  g.panel = panel, g.screen = screen, g.state = state, g.tab = tab;
  return true;
}

// The list the panel shows, row by row (what the jump walks): the rows with their letters / selectability, the ids, the current row.
struct PanelView {
  uint32_t list = 0, cur = 0, mgr = 0;
  std::vector<Row> rows;
  std::vector<uint32_t> ids;
};

inline bool CollectRows(scene_director::Caller& call, uint8_t* base, const Gated& g, PanelView& out) {
  const uint32_t panel = g.panel, screen = g.screen, tab = g.tab;
  uint32_t list = 0, count = 0, cur = 0;
  if (!Rd(base, panel + kListShown, list) || list > 5) return false;
  const uint32_t lbase = panel + kListCount + kListStride * list;
  if (!Rd(base, lbase, count) || count < 2 || count > uint32_t(kMaxRows)) return false;
  cur = call(sub_828568B8, {panel});  // the game's own current index: the one sub_8285BD18 turns into the participant
  uint32_t mgr = 0;
  if (!Rd(base, kTextManagerPtr, mgr) || !seats::Plausible(mgr)) return false;
  if (!g_scratch) g_scratch = seats::Alloc(nullptr, 16);

  out.rows.clear();
  out.ids.clear();
  out.rows.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t w = 0;
    if (!Rd(base, lbase + 4 + 4 * i, w)) return false;
    const uint32_t id = w & 0x7FFFFFFFu;
    out.ids.push_back(id);
    Row r;
    r.index = int(i);
    r.selectable = Selectable(base, call, panel, screen, tab, id);
    r.letter = (r.selectable || i == cur) ? Initial(NameOf(base, call, id, mgr)) : char(0);
    out.rows.push_back(r);
  }
  out.list = list, out.cur = cur, out.mgr = mgr;
  return true;
}

// S110 LETTERUI: what the letter strip shows for a panel: the letters present (ascending: the ones the jump lands on) and the current row's letter, from the
// same rows the jump uses. false = the panel is not in a state where the jump works (no strip then).
struct PanelLetters {
  std::vector<char> present;
  char cur = 0;
  uint32_t tab = 0;
};

inline bool LettersOf(scene_director::Caller& call, uint8_t* base, uint32_t panel, PanelLetters& out) {
  Gated g;
  PanelView v;
  if (!GateOf(call, base, panel, g) || !CollectRows(call, base, g, v)) return false;
  out.present = PresentLetters(v.rows);
  out.cur = (v.cur < v.rows.size()) ? v.rows[v.cur].letter : char(0);
  out.tab = g.tab;
  return true;
}

// S110 SEAT3: the downloaded-tab pager (src\seat_bind.h) asks the same rows the jump walks, to know which letters the list holds, and uses the same
// "cursor to row N" pair after it rebuilt the list. Thin adapters over GateOf's fields / CollectRows, so there is one row walk.
using Gathered = PanelView;
inline bool GatherRows(uint8_t* base, scene_director::Caller& call, uint32_t panel, uint32_t screen, uint32_t tab, Gathered& g) {
  Gated gate;
  gate.panel = panel, gate.screen = screen, gate.tab = tab;
  return CollectRows(call, base, gate, g);
}

// The game's own "cursor to row N" pair (see the top note): what a jump does, and what the pager does after it rebuilt the list.
inline void MoveCursor(scene_director::Caller& call, uint32_t panel, uint32_t row) {
  call(sub_8285A160, {panel, uint64_t(row)});
  call(sub_8285BD18, {panel, 0});
}

// One LT (dir -1) / RT (dir +1) press. true = the cursor was moved (or a provider took the press).
inline bool OnTrigger(PPCContext& ctx, uint8_t* base, uint32_t mixin, int dir) {
  if (!LetterJumpOn()) return false;
  const uint32_t panel = mixin - 44;
  scene_director::Caller call(ctx, base);
  Gated g;
  if (!GateOf(call, base, panel, g)) return false;
  if (!g_logged_first.exchange(true))
    REXLOG_INFO("[wwe2k14][letter] hook first reached: panel {:08X} screen {:08X} state {} dir {}", panel, g.screen, g.state, dir);
  if (g_page_provider && g_page_provider(panel, dir, ctx, base)) return true;
  PanelView v;
  if (!CollectRows(call, base, g, v)) return false;
  const std::vector<Row>& rows = v.rows;
  const uint32_t cur = v.cur;
  const int n = NextRow(rows, int(cur), dir);
  if (n < 0 || size_t(n) >= rows.size()) return false;
  const char from = (cur < rows.size()) ? rows[cur].letter : char(0);
  MoveCursor(call, panel, uint32_t(n));
  REXLOG_INFO("[wwe2k14][letter] panel {:08X} tab {} list {}: '{}' -> '{}' row {} id {} ({})", panel, g.tab, v.list, from ? from : '?', rows[n].letter, n,
              v.ids[n], NameOf(base, call, v.ids[n], v.mgr));
  return true;
}

inline void Install() {
  REXLOG_INFO("[wwe2k14][letter] letter jump: {} (--wwe2k14_letter_jump)", LetterJumpOn() ? "on" : "off");
}

}  // namespace wwe2k14::letter_jump

// ---------------------------------------------------------------------------------------------------------------- the hooks
REX_HOOK_RAW(sub_823BC468) {  // LT
  wwe2k14::letter_jump::OnTrigger(ctx, base, ctx.r3.u32, -1);
  __imp__sub_823BC468(ctx, base);
}

REX_HOOK_RAW(sub_823BC5D0) {  // RT
  wwe2k14::letter_jump::OnTrigger(ctx, base, ctx.r3.u32, +1);
  __imp__sub_823BC5D0(ctx, base);
}
