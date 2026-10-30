// wwe2k14 - the mouse in the game's list menus (Session 67; Jarrod: "let's have it so the menu items can be highlighted
// by the mouse"). --wwe2k14_mouse_menus (default on).
//
// The lists of the main-menu system (the main menu, PLAY, CREATIONS, OPTIONS, ...: the SmartMenu, docs\systems\
// menu_ui.md / menus.md) draw through a fixed set of row slots in one panel. Found with the label probe (run 042,
// probes\probes.cpp Session 67 + tools\menus\label_dump.py):
//   - sub_8283B118(r3 = the row table R, r4 = the slot index, r5 = the text id) fills one filled row every frame (R+4 =
//     the table's capacity, 20; the main menu fills 9 slots: 8 rows in the panel and one below it for the scroll); it
//     sets the text through the label element's method sub_82B53068(r3 = the element), called from inside it.
//   - The element's world matrix is at +704..+767: the translation (the row box's centre, in the game's 1280 x 720
//     screen: the layout root is 1280 x 720) at +768 / +772; the drawn box size at +448 / +452 (295.8 x 24 in the main
//     menu; +416.. is its scale, 1). Checked against a screenshot: every row's centre y within 1 px, the box's left
//     edge 3 px left of the text.
//   - The SmartMenu ([0x8392945C]): the cursor (item index) at +352, the scroll (the item in slot 0) at +1388 (0 at the
//     top of the main menu, 1 with QUIT TO DESKTOP highlighted; the slots then hold items 1-8).
// So the item under the mouse = scroll + the slot whose box holds it. The mouse moves the game's own cursor there, only
// when it moves onto another item (a still mouse never fights the pad): a jump of more than one row puts the cursor on
// the target's neighbour and one UP / DOWN press lets the game's own move land on the target (its sound, highlight,
// action code and preview); a left click on an item presses A once the cursor is on it; the wheel presses UP / DOWN;
// a right click presses B anywhere in the main-menu system (lists and the screens opened from them: back).
// Presses: one frame pressed, one released (the fastest the list takes, run 4350). Off while the keyboard-as-pad mapping holds the mouse for the right
// stick (mnk_mode + mnk_mouse), while an input script plays, while a screen (menu id >= 1000) or a message box is over
// the list, and never in a match (Jarrod's standing rule, Session 67). The window's picture is 16:9, centred (letterboxed) unless present_letterbox is off.
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame_stats.h"   // g_guest_frame (VdSwap count)
#include "guest_read.h"    // GuestWordReadableFast
#include "input_script.h"  // g_mouse_pad_filter

REXCVAR_DEFINE_BOOL(wwe2k14_mouse_menus, true, "wwe2k14",
                    "Session 67: the mouse in the game's list menus (main menu, PLAY, CREATIONS, OPTIONS, ...): "
                    "pointing at an item moves the menu's cursor to it, a left click selects it (A), the wheel moves "
                    "up / down, a right click goes back (B; also on the screens opened from the lists)");
REXCVAR_DEFINE_BOOL(wwe2k14_mouse_menus_log, false, "wwe2k14",
                    "Session 67: log each pointer move onto an item and each press --wwe2k14_mouse_menus makes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(wwe2k14_mouse_menus_dump, false, "wwe2k14",
                    "Session 67 (debug): set it (live: cvar wwe2k14_mouse_menus_dump true) to log, once, every text "
                    "element on screen with its text id, text, box and parent (for mapping a new screen)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace wwe2k14::mouse_menu {

constexpr uint32_t kSmartMenuPtr = 0x8392945C;
constexpr uint32_t kMessageManagerPtr = 0x839294BC, kMessageStateOffset = 16;
// The sequence manager: [[0x839400F8]+0]+48 = the current sequence id, 9 = in game (docs\systems\menu_ui.md).
constexpr uint32_t kSequenceManagerPtr = 0x839400F8, kSequenceIdOffset = 48, kSequenceInGame = 9;
constexpr uint32_t kCursorOffset = 352, kScrollOffset = 1388, kMenuOffset = 364;
constexpr uint32_t kElemX = 768, kElemY = 772, kElemW = 448, kElemH = 452;
// The panel shows 8 rows: the main menu's 9th item (QUIT TO DESKTOP) sits in a slot below it and the list scrolls
// after the 8th (run 042); CREATIONS shows 8 of its 9 items the same way (Session 27 screenshots).
constexpr int kPanelRows = 8;
constexpr float kUiW = 1280, kUiH = 720;
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadA = 0x1000, kPadB = 0x2000;
// A press set in frame N is seen from the next frame's input poll (after VdSwap) until release_frame: hold 2 = seen in
// one frame; gap 0 = the next press is set in N + 2, seen in N + 3. One frame pressed, one released took every press
// (run 4350: 6 presses, 6 rows); 2 held + 3 released (the first version) made a fixed pace Jarrod felt as lag.
constexpr int kHoldFrames = 2, kGapFrames = 0;
constexpr int kMaxPresses = 40;
constexpr int kMaxSlots = 16;

struct Box {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool set = false;
};

struct State {
  // Game thread (the row hooks): this frame's rows, built slot by slot, then published.
  int slot_count = 0;
  std::array<Box, kMaxSlots> building{};
  std::mutex mutex;                 // guards `rows` / `rows_visible` (published once a frame, read by the UI thread)
  std::array<Box, kMaxSlots> rows{};
  int rows_visible = 0;
  std::atomic<int64_t> seen_ms{0};  // when the rows were last published
  // The mouse (UI thread -> game thread): the item to move to, a pending click, wheel presses.
  std::atomic<int> target{-1};
  std::atomic<bool> select{false};
  std::atomic<int> wheel{0};        // + = DOWN presses wanted, - = UP
  std::atomic<uint64_t> back_until{0};  // right click: B is held until this guest frame (not counting it)
  int hover_item = -1;              // UI thread: the item under the mouse at the last move (-1 = none), in the list
  uint32_t hover_menu = 0;          // the open list then (SmartMenu +364)
  // Game thread: the press in progress (guest frames).
  uint64_t build_frame = 0;         // the frame whose rows `building` holds
  uint64_t next_frame = 0;          // no new press before this frame
  std::atomic<uint64_t> release_frame{0};  // the press is held until this frame
  int active_target = -2, presses = 0;
  std::atomic<uint16_t> buttons{0};
  std::atomic<uint64_t> moves{0}, selects{0};
  uint32_t logged_menu = 0xFFFFFFFF;  // game thread: the list whose rows were logged last
  rex::ui::Window* window = nullptr;
};

inline State& S() {
  static State s;
  return s;
}

// Inside sub_8283B118 on this thread (set-text runs on every thread): in a row call, and the box its set-text gave.
inline thread_local bool t_in_row = false;
inline thread_local Box t_row{};

inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline uint8_t* Guest(uint32_t a) { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a); }
inline uint32_t Read32(uint32_t a) { return rex::memory::load_and_swap<uint32_t>(Guest(a)); }
inline float ReadF(uint32_t a) {
  const uint32_t w = Read32(a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
inline uint32_t SmartMenu() { return Read32(kSmartMenuPtr); }
inline bool ListUp() { return S().seen_ms.load() && NowMs() - S().seen_ms.load() < 150; }

// In a match (Jarrod, Session 67: "we shouldn't use any mouse controls in matches"). The SmartMenu does not exist
// there anyway (menu_ui.md); this says it outright.
// The current sequence's id (75 = the main menu, 9 = in game, 76 = Create a Superstar, 35 = 30 Years of WrestleMania;
// docs\systems\menu_ui.md), 0 if none.
inline uint32_t SequenceId() {
  const uint32_t mgr = Read32(kSequenceManagerPtr);
  const uint32_t seq = mgr ? Read32(mgr) : 0;
  return seq ? Read32(seq + kSequenceIdOffset) : 0;
}
inline bool InMatch() { return SequenceId() == kSequenceInGame; }

// The list is what the player sees: its menu id is a list (< 1000; 1000 and up are screens such as the match setup or
// GRAPHICS, drawn over the list, whose rows the game keeps setting underneath: run 4343, menu 1300) and no message box
// is busy (the message manager's state +16: 5 idle, 0 none; 1 opening, 2 waiting, 3 answered: docs\systems\menus.md).
inline bool ListActive(uint32_t sm) {
  if (!sm || Read32(sm + kMenuOffset) >= 1000) return false;
  const uint32_t mm = Read32(kMessageManagerPtr);
  const uint32_t st = mm ? Read32(mm + kMessageStateOffset) : 0;
  return st == 0 || st == 5;
}

// Game thread, once per guest frame (VdSwap count, frame_stats.h) in which the list's rows were set: publish the last
// frame's rows, then the next step of a press toward the target. (Run 4340: counting frames by the slot index coming
// round again gave several "frames" in one while the cursor moved: presses 10 ms apart.)
inline void OnMenuFrame(uint64_t frame) {
  State& s = S();
  {
    std::lock_guard lock(s.mutex);
    s.rows = s.building;
    s.rows_visible = std::min(s.slot_count, kPanelRows);
  }
  if (const uint32_t sm0 = SmartMenu(); sm0 && Read32(sm0 + kMenuOffset) != s.logged_menu && s.building[0].set &&
      s.building[0].x1 - s.building[0].x0 > 2 && s.building[0].y0 > 1) {  // (a list's first frames have no layout)
    s.logged_menu = Read32(sm0 + kMenuOffset);
    const Box& b = s.building[0];
    REXLOG_INFO("[wwe2k14][mouse] list {}: {} slots in its table, {} rows in the panel; row 0 box ({:.0f},{:.0f})-"
                "({:.0f},{:.0f})", s.logged_menu, s.slot_count, s.rows_visible, b.x0, b.y0, b.x1, b.y1);
  }
  s.building = {};
  s.seen_ms = NowMs();
  if (frame < s.next_frame) return;  // a press held, then released
  const uint32_t sm = SmartMenu();
  if (!ListActive(sm) || g_input_script.Busy() || InMatch()) {  // a screen / message box over the list, a script: idle
    s.target = -1;
    s.select = false;
    s.wheel = 0;
    return;
  }
  uint16_t press = 0;
  if (const int w = s.wheel.load(); w != 0) {
    s.wheel.fetch_sub(w > 0 ? 1 : -1);
    press = w > 0 ? kPadDown : kPadUp;
  } else if (const int target = s.target.load(); target >= 0) {
    if (target != s.active_target) s.active_target = target, s.presses = 0;
    const int cursor = int(Read32(sm + kCursorOffset));
    if (cursor == target) {
      int expected = target;
      s.target.compare_exchange_strong(expected, -1);  // (unless the mouse already asked for another)
      if (s.select.exchange(false)) press = kPadA, ++s.selects;
    } else if (++s.presses > kMaxPresses) {  // never got there: give up (a list we read wrong)
      s.target = -1;
      s.select = false;
      return;
    } else {
      // More than one row away: the cursor goes on the row next to the target (on screen, so the move does not scroll)
      // and one press lets the game's own move finish onto it (sub_82842C08 stores the cursor, plays the sound, looks
      // up the item's action code +368, updates the preview and the highlight; Session 67 listing + watch, run 4350).
      // A jump costs one press instead of one per row.
      int from = cursor;
      if (target - cursor > 1 || cursor - target > 1) {
        const int scroll = int(Read32(sm + kScrollOffset));
        from = target > cursor ? target - 1 : target + 1;
        if (from < scroll) from = target + 1;
        if (from >= scroll + kPanelRows) from = target - 1;
        rex::memory::store_and_swap<uint32_t>(Guest(sm + kCursorOffset), uint32_t(from));
      }
      press = target > from ? kPadDown : kPadUp;
      ++s.moves;
    }
  }
  if (!press) return;
  if (REXCVAR_GET(wwe2k14_mouse_menus_log))
    REXLOG_INFO("[wwe2k14][mouse] frame {}: press {} (cursor {}, target {})", frame,
                press == kPadA ? "A" : press == kPadUp ? "UP" : "DOWN", int(Read32(sm + kCursorOffset)), s.target.load());
  s.buttons = press;
  s.release_frame = frame + kHoldFrames;
  s.next_frame = frame + kHoldFrames + kGapFrames;
}

// Game thread (XamInputGetState, player 1): our press on top of the real pad, only while the list still draws (A
// opens another screen: the press must not stay held into it).
inline void GridPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame);
inline void SlotPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame);
inline void ListPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame);
inline void ArenaPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame);
inline void LegendPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame);
inline void DumpTextElements();
inline void PadFilter(rex::input::X_INPUT_STATE* state) {
  State& s = S();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (REXCVAR_GET(wwe2k14_mouse_menus_dump)) {
    REXCVAR_SET(wwe2k14_mouse_menus_dump, false);
    DumpTextElements();
  }
  if (const uint16_t add = s.buttons.load(); add && frame < s.release_frame && NowMs() - s.seen_ms.load() < 40)
    state->gamepad.buttons = uint16_t(state->gamepad.buttons | add);
  if (frame < s.back_until.load()) state->gamepad.buttons = uint16_t(state->gamepad.buttons | kPadB);  // right click
  GridPadFilter(state, frame);  // the superstar grid's jump / A
  SlotPadFilter(state, frame);  // the player slots' moves / A
  ListPadFilter(state, frame);  // the UI lists' jump / A
  ArenaPadFilter(state, frame);  // the arena tiles' jump / A
  LegendPadFilter(state, frame);  // the legend's A / B clicked
}

// A keyboard-mode bind on the right mouse button (keybind_* holding "RMB"): then the right click is the player's own.
inline bool RightButtonBound() {
  if (rex::cvar::GetFlagByName("mnk_mode") != "true") return false;
  for (const std::string& name : rex::cvar::ListFlags())
    if (name.rfind("keybind_", 0) == 0 && rex::cvar::GetFlagByName(name).find("RMB") != std::string::npos) return true;
  return false;
}

// The keyboard-as-pad mapping holds the mouse for the right stick: leave the mouse to it. Also while an input script
// plays (harness routes; the live channel's batches): it has the controller, a stray pointer must not move its menus.
// And never in a match.
inline bool MouseTaken() {
  return (rex::cvar::GetFlagByName("mnk_mode") == "true" && rex::cvar::GetFlagByName("mnk_mouse") == "true") ||
         g_input_script.Busy() || InMatch();
}

// UI thread: the window pixel (x, y) in the game's 1280 x 720 screen; false outside the picture.
inline bool ToUi(int32_t x, int32_t y, float& ux, float& uy) {
  rex::ui::Window* w = S().window;
  if (!w) return false;
  const float ww = float(w->GetActualPhysicalWidth()), wh = float(w->GetActualPhysicalHeight());
  if (ww <= 0 || wh <= 0) return false;
  // Session 72 part 9: with --wwe2k14_aspect wider / taller than 16:9 (src\ultrawide.h) the presenter stretches the
  // frame, but the menus are squeezed into it first: on screen they are the centred 16:9 again.
  auto factor = [](const char* n) { return std::strtod(rex::cvar::GetFlagByName(n).c_str(), nullptr); };
  if (rex::cvar::GetFlagByName("present_letterbox") == "false" && factor("gpu_wide_factor") <= 1.001 &&
      factor("gpu_tall_factor") <= 1.001) {
    ux = float(x) * kUiW / ww, uy = float(y) * kUiH / wh;
  } else {
    const float k = std::min(ww / kUiW, wh / kUiH), ox = (ww - kUiW * k) / 2, oy = (wh - kUiH * k) / 2;
    ux = (float(x) - ox) / k, uy = (float(y) - oy) / k;
  }
  return ux >= 0 && uy >= 0 && ux < kUiW && uy < kUiH;
}

// UI thread: the list item under the window pixel, or -1 (and the open list's id).
inline int ItemAt(int32_t x, int32_t y, uint32_t& menu) {
  float ux = 0, uy = 0;
  menu = 0;
  if (!ListUp() || !ToUi(x, y, ux, uy)) return -1;
  const uint32_t sm = SmartMenu();
  if (!ListActive(sm)) return -1;
  menu = Read32(sm + kMenuOffset);
  State& s = S();
  std::lock_guard lock(s.mutex);
  for (int i = 0; i < s.rows_visible; ++i) {
    const Box& b = s.rows[size_t(i)];
    if (b.set && ux >= b.x0 && ux < b.x1 && uy >= b.y0 && uy < b.y1) return int(Read32(sm + kScrollOffset)) + i;
  }
  return -1;
}

// ---- Session 67, part 3: the superstar select grid (setup screen 2000; docs\systems\menus.md "superstar select").
// The portrait grid is a widget of the game's shared grid input (row +20, column +36: the same base as the on-screen
// keyboard). Its per-frame copy sub_82856B40(W) runs while it has the input (W = the grid base; [W+220] = the select
// screen, [W+232] the player choosing); a move goes W's "cursor moved" -> sub_828584F0(W-44) -> sub_8282F550(screen,
// player, &column, &row): the start position comes in through the two pointers, the press's direction walks to the
// next selectable cell (sub_8282C648), the result goes back through them (run 4352, listings). A jump: one press with
// the start set to the target itself (the walk tests its start first, skipping only the cell the cursor is on), so the
// game's own move lands on the target (its preview, frame and sound). Selectable: the cell's character id sub_8293A180(table, column,
// row) is not 512 (empty) and sub_8282C298(model, id) says yes; model = [screen + 56796], table = [[model+16] + 56792].
// Cell centres measured on the screen (1280 x 720): x = 199 + 44.05 * (column - 1), y = 409 + 50 * row, rows 0-3; the
// DIVAS tab's grid sits on the same lattice (AJ Lee: column 9 at x 551); the tab row is row 4 (kTabs).
constexpr float kGridX0 = 199.0f, kGridDx = 44.05f, kGridY0 = 409.0f, kGridDy = 50.0f, kCellHalfW = 22.0f,
                kCellHalfH = 25.0f;
constexpr int kGridRows = 4, kGridMaxCol = 22;
constexpr uint32_t kGridScreenOffset = 220, kGridRowOffset = 20, kGridColOffset = 36, kScreenModelOffset = 56796,
                   kModelTableHolderOffset = 16, kTableOffset = 56792, kEmptyCell = 512;
constexpr uint16_t kPadLeft = 0x0004, kPadRight = 0x0008;

struct Grid {
  std::atomic<uint32_t> widget{0};   // W, the grid seen last (game thread)
  std::atomic<int64_t> seen_ms{0};   // when its per-frame copy last ran
  std::atomic<int> target_col{-1}, target_row{-1};  // UI -> game: the cell to jump to (-1 = none)
  std::atomic<bool> select{false};
  int hover_col = -2, hover_row = -2;  // UI thread: the cell under the pointer at the last move
  // Game thread.
  uint64_t next_frame = 0;
  std::atomic<uint64_t> release_frame{0};
  std::atomic<uint16_t> buttons{0};
  bool jump_pending = false;
  int jump_col = 0, jump_row = 0, active_col = -1, active_row = -1, tries = 0;
  uint32_t jump_screen = 0;
};

inline Grid& G() {
  static Grid g;
  return g;
}

inline bool GridUp() { return G().seen_ms.load() && NowMs() - G().seen_ms.load() < 150; }

// The tab row (row 4): the walk snaps columns 0-2 / 3-5 / 17-19 / 20-22 to the tabs at 0 CUSTOM SUPERSTARS, 3
// DOWNLOADED SUPERSTARS, 17 DIVAS (SUPERSTARS while the divas show), 20 MANAGERS and takes those outright (listing
// 0x8282C7EC-0x8282C908). Boxes measured on the screen (run 4352).
struct Tab {
  float x0, x1;
  int col;
};
constexpr Tab kTabs[] = {{135, 262, 0}, {266, 396, 3}, {882, 1012, 17}, {1016, 1146, 20}};
constexpr float kTabY0 = 588, kTabY1 = 634;
constexpr int kTabRow = 4;

// UI thread: the grid cell (or tab) under the game-screen point, or false.
inline bool CellAt(float ux, float uy, int& col, int& row) {
  if (uy >= kTabY0 && uy < kTabY1) {
    for (const Tab& t : kTabs)
      if (ux >= t.x0 && ux < t.x1) return col = t.col, row = kTabRow, true;
    return false;
  }
  row = int(std::lround((uy - kGridY0) / kGridDy));
  col = int(std::lround((ux - kGridX0) / kGridDx)) + 1;
  if (row < 0 || row >= kGridRows || col < 0 || col > kGridMaxCol) return false;
  return std::fabs(ux - (kGridX0 + kGridDx * float(col - 1))) <= kCellHalfW &&
         std::fabs(uy - (kGridY0 + kGridDy * float(row))) <= kCellHalfH;
}

// Game thread: a guest call with word arguments (the registers put back after).
inline uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t a0, uint32_t a1,
                          uint32_t a2 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = a0, ctx.r4.u64 = a1, ctx.r5.u64 = a2;
  ctx.lr = 0;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

}  // namespace wwe2k14::mouse_menu

REX_EXTERN(sub_8293A180);  // (table, column, row) -> the cell's character id, 512 = empty
REX_EXTERN(sub_8282C298);  // (model, id) -> 1 if the id can be picked here

namespace wwe2k14::mouse_menu {

inline bool CellSelectable(PPCContext& ctx, uint8_t* base, uint32_t w, int col, int row) {
  if (row == kTabRow) return col == 0 || col == 3 || col == 17 || col == 20;  // the walk takes the tabs outright
  const uint32_t screen = Read32(w + kGridScreenOffset);
  const uint32_t model = screen ? Read32(screen + kScreenModelOffset) : 0;
  const uint32_t holder = model ? Read32(model + kModelTableHolderOffset) : 0;
  const uint32_t table = holder ? Read32(holder + kTableOffset) : 0;
  if (!table) return false;
  const uint32_t id = CallGuest(ctx, base, sub_8293A180, table, uint32_t(col), uint32_t(row));
  return id != kEmptyCell && CallGuest(ctx, base, sub_8282C298, model, id) != 0;
}

// Game thread, once a frame while the grid has the input (sub_82856B40's hook): the next step toward the target.
inline void OnGridFrame(PPCContext& ctx, uint8_t* base, uint32_t w) {
  Grid& g = G();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (!GridUp())  // (it had not run for 150 ms: the grid got the input again)
    REXLOG_INFO("[wwe2k14][mouse] superstar grid has the input (widget {:08X}, column {}, row {})", w,
                int(Read32(w + kGridColOffset)), int(Read32(w + kGridRowOffset)));
  g.widget = w;
  g.seen_ms = NowMs();
  if (frame < g.next_frame) return;
  const int tc = g.target_col.load(), tr = g.target_row.load();
  if (tc < 0) return;
  if (!SmartMenu() || g_input_script.Busy() || InMatch()) {
    g.target_col = g.target_row = -1;
    g.select = false;
    return;
  }
  if (tc != g.active_col || tr != g.active_row) g.active_col = tc, g.active_row = tr, g.tries = 0;
  const int col = int(Read32(w + kGridColOffset)), row = int(Read32(w + kGridRowOffset));
  uint16_t press = 0;
  if (col == tc && row == tr) {
    int expected = tc;
    if (g.target_col.compare_exchange_strong(expected, -1)) g.target_row = -1;
    if (g.select.exchange(false)) press = kPadA;
  } else if (++g.tries > 3 || !CellSelectable(ctx, base, w, tc, tr)) {  // empty cell, or it did not land: leave it
    if (REXCVAR_GET(wwe2k14_mouse_menus_log))
      REXLOG_INFO("[wwe2k14][mouse] grid: column {} row {} not reached (tries {}); cursor at {} / {}", tc, tr, g.tries,
                  col, row);
    g.target_col = g.target_row = -1;
    g.select = false;
    return;
  } else {
    // The walk (sub_8282C648) tests its start cell first and only skips the player's own current cell (listing
    // 0x8282C95C-0x8282CA9C), so the start is the target itself (run 4354: a start on the left neighbour stopped there,
    // one to the left: Jarrod "it's offset").
    g.jump_col = tc;
    g.jump_row = tr;
    g.jump_screen = Read32(w + kGridScreenOffset);
    g.jump_pending = true;
    press = kPadRight;
  }
  if (!press) return;
  if (REXCVAR_GET(wwe2k14_mouse_menus_log))
    REXLOG_INFO("[wwe2k14][mouse] grid frame {}: press {} (cursor {} / {}, target {} / {})", frame,
                press == kPadA ? "A" : press == kPadRight ? "RIGHT" : "LEFT", col, row, tc, tr);
  g.buttons = press;
  g.release_frame = frame + kHoldFrames;
  g.next_frame = frame + kHoldFrames + 1;  // (the move is in the next frame; check it the frame after)
}

inline void GridPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame) {
  Grid& g = G();
  if (const uint16_t add = g.buttons.load(); add && frame < g.release_frame && NowMs() - g.seen_ms.load() < 60)
    state->gamepad.buttons = uint16_t(state->gamepad.buttons | add);
}

// ---- Session 67, part 4: the player slots of the select screens (1P / COM; Jarrod's 6-man screen, runs 4356-4360).
// Which slot the player's controller sits in is not a number: each participant's record (sub_82921ED8(manager, index)
// = manager + [manager + 11000] * 1100 + (index + 1) * 156, manager = [0x839294C0]) holds the controller driving it
// at +60 (-1 = COM): the player's slot is the participant whose +60 is 0 (run 4356: L0 -> 0, one DOWN -> 2, RIGHT -> 3).
// Participants are numbered left to right, row by row (Jarrod: "it might be left to right"). The slot choice runs
// sub_82856E58 once per slot a frame (its panel: [panel + 276] = the participant; hooked: the mode and the slot count).
// Where the slots are on screen is not in any object found (the label objects, the panels, their page and layout node
// hold no position, runs 4358-4360), so the layouts are measured from screenshots (run 4358, 1280 x 720: label row
// centres; a slot's bar spans y - 36 .. y + 18, the left column x 120-600, the right 680-1150, triple threat's top slot
// centred 370-830) and chosen by the setup screen's menu id (2000 + the match type; families from
// db\ids\match_types.json "sides": the first type of each family seen). Other families (6-man tag, handicaps, the
// Elimination Chamber, Royal Rumbles) have no slot hover until measured; a layout is used only while the number of slot
// panels drawn matches it. A hover steps the controller there with LEFT / RIGHT (the other column) and UP / DOWN (another
// row), one press at a time, looking again after each.
constexpr uint32_t kParticipantManagerPtr = 0x839294C0, kParticipantBlockOffset = 11000, kParticipantStride = 156,
                   kParticipantBlockStride = 1100, kParticipantControllerOffset = 60, kPanelParticipantOffset = 276;
constexpr uint16_t kPadUpDir = 0x0001, kPadDownDir = 0x0002;

struct SlotPos {
  float x0, x1, y;  // the slot's bar (x) and its label row's centre (y)
};
struct SlotLayout {
  int count;
  SlotPos slot[6];
};
constexpr float kSlotLx0 = 120, kSlotLx1 = 600, kSlotRx0 = 680, kSlotRx1 = 1150, kSlotCx0 = 370, kSlotCx1 = 830;
constexpr SlotLayout kOneOnOne = {2, {{kSlotLx0, kSlotLx1, 341}, {kSlotRx0, kSlotRx1, 341}}};
constexpr SlotLayout kTag = {4, {{kSlotLx0, kSlotLx1, 286}, {kSlotRx0, kSlotRx1, 286}, {kSlotLx0, kSlotLx1, 342},
                                 {kSlotRx0, kSlotRx1, 342}}};
constexpr SlotLayout kTriple = {3, {{kSlotCx0, kSlotCx1, 314}, {kSlotLx0, kSlotLx1, 370}, {kSlotRx0, kSlotRx1, 370}}};
constexpr SlotLayout kFour = {4, {{kSlotLx0, kSlotLx1, 314}, {kSlotRx0, kSlotRx1, 314}, {kSlotLx0, kSlotLx1, 370},
                                  {kSlotRx0, kSlotRx1, 370}}};
constexpr SlotLayout kSix = {6, {{kSlotLx0, kSlotLx1, 258}, {kSlotRx0, kSlotRx1, 258}, {kSlotLx0, kSlotLx1, 314},
                                 {kSlotRx0, kSlotRx1, 314}, {kSlotLx0, kSlotLx1, 370}, {kSlotRx0, kSlotRx1, 370}}};
// Match types by family (db\ids\match_types.json: ONE ON ONE / "01", "0101" + TAG TEAM, "012" + TRIPLE THREAT >
// NORMAL, "0123" + FATAL 4-WAY > NORMAL, "012345"; 6-man tag "010101": the same six rows, 1P top left, the others
// COM in record order, run 4361 screenshot + records at menu 2008).
constexpr int kOneOnOneTypes[] = {0, 26, 33, 49, 54, 55, 59, 60, 67, 68, 72, 77, 81};
constexpr int kTagTypes[] = {3, 4, 7, 9, 10, 12, 29, 50, 56, 61, 69, 73, 78};
constexpr int kTripleTypes[] = {13, 30, 51, 57, 62, 70, 74, 79};
constexpr int kFourTypes[] = {14, 31, 34, 52, 58, 63, 71, 75, 80};
constexpr int kSixTypes[] = {35, 53, 76, 8, 11};

inline const SlotLayout* LayoutForMenu(uint32_t menu) {
  if (menu < 2000 || menu >= 2200) return nullptr;
  const int t = int(menu) - 2000;
  auto in = [t](const auto& list) { return std::find(std::begin(list), std::end(list), t) != std::end(list); };
  if (in(kOneOnOneTypes)) return &kOneOnOne;
  if (in(kTagTypes)) return &kTag;
  if (in(kTripleTypes)) return &kTriple;
  if (in(kFourTypes)) return &kFour;
  if (in(kSixTypes)) return &kSix;
  return nullptr;
}

struct Slots {
  std::atomic<int64_t> seen_ms{0};  // the slot choice's handler last ran
  uint64_t count_frame = 0;         // game thread: panels drawn this frame / the last
  int calls_this_frame = 0;
  std::atomic<int> panels{0};
  // UI -> game.
  std::atomic<int> target{-1};
  std::atomic<bool> select{false};
  int hover = -2;  // UI thread
  // Game thread presses.
  uint64_t next_frame = 0;
  std::atomic<uint64_t> release_frame{0};
  std::atomic<uint16_t> buttons{0};
  int active_target = -1, tries = 0;
};

inline Slots& SL() {
  static Slots s;
  return s;
}

inline bool SlotsUp() { return SL().seen_ms.load() && NowMs() - SL().seen_ms.load() < 150; }

// The layout of the screen now, if it is one we measured and its slot count matches the panels drawn.
inline const SlotLayout* CurrentLayout() {
  const uint32_t sm = SmartMenu();
  const SlotLayout* l = sm ? LayoutForMenu(Read32(sm + kMenuOffset)) : nullptr;
  return l && l->count == SL().panels.load() ? l : nullptr;
}

// The participant the player's controller (0) drives, or -1.
inline int PlayerSlot(int count) {
  const uint32_t mgr = Read32(kParticipantManagerPtr);
  if (!mgr) return -1;
  const uint32_t first = mgr + Read32(mgr + kParticipantBlockOffset) * kParticipantBlockStride;
  for (int i = 0; i < count; ++i)
    if (Read32(first + uint32_t(i + 1) * kParticipantStride + kParticipantControllerOffset) == 0) return i;
  return -1;
}

// Game thread, once a frame while the slot choice has the input: the next press toward the target.
inline void OnSlotFrame() {
  Slots& sl = SL();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (frame < sl.next_frame) return;
  const int target = sl.target.load();
  if (target < 0) return;
  const SlotLayout* layout = CurrentLayout();
  const int cur = layout ? PlayerSlot(layout->count) : -1;
  if (!layout || cur < 0 || target >= layout->count || !SmartMenu() || GridUp() || g_input_script.Busy() ||
      InMatch()) {
    sl.target = -1;
    sl.select = false;
    return;
  }
  if (target != sl.active_target) sl.active_target = target, sl.tries = 0;
  uint16_t press = 0;
  if (cur == target) {
    int expected = target;
    sl.target.compare_exchange_strong(expected, -1);
    if (sl.select.exchange(false)) press = kPadA;
  } else if (++sl.tries > 8) {
    sl.target = -1;
    sl.select = false;
    return;
  } else {
    const SlotPos &a = layout->slot[cur], &b = layout->slot[target];
    const float ax = (a.x0 + a.x1) / 2, bx = (b.x0 + b.x1) / 2;
    if ((ax < 640) != (bx < 640)) press = bx < ax ? kPadLeft : kPadRight;  // the other column
    else if (std::fabs(b.y - a.y) > 4) press = b.y < a.y ? kPadUpDir : kPadDownDir;
    else press = bx < ax ? kPadLeft : kPadRight;
  }
  if (!press) return;
  if (REXCVAR_GET(wwe2k14_mouse_menus_log))
    REXLOG_INFO("[wwe2k14][mouse] slots frame {}: press {:04X} (player in slot {}, target {}, {} slots)", frame, press,
                cur, target, layout->count);
  sl.buttons = press;
  sl.release_frame = frame + kHoldFrames;
  sl.next_frame = frame + kHoldFrames + 1;
}

// Game thread, each call of the slot choice's handler (one per slot a frame).
inline void OnSlotHandler() {
  Slots& sl = SL();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (frame != sl.count_frame) {  // the first slot of a new frame
    if (sl.count_frame) sl.panels = sl.calls_this_frame;
    sl.calls_this_frame = 0;
    sl.count_frame = frame;
    if (!SlotsUp()) REXLOG_INFO("[wwe2k14][mouse] player slots have the input");
    sl.seen_ms = NowMs();
    OnSlotFrame();
  }
  ++sl.calls_this_frame;
}

inline void SlotPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame) {
  Slots& sl = SL();
  if (const uint16_t add = sl.buttons.load(); add && frame < sl.release_frame && NowMs() - sl.seen_ms.load() < 60)
    state->gamepad.buttons = uint16_t(state->gamepad.buttons | add);
}

// UI thread: the slot under the game-screen point, or -1.
inline int SlotAt(float ux, float uy) {
  const SlotLayout* layout = CurrentLayout();
  if (!layout) return -1;
  for (int i = 0; i < layout->count; ++i) {
    const SlotPos& p = layout->slot[i];
    if (ux >= p.x0 && ux < p.x1 && uy >= p.y - 36 && uy < p.y + 18) return i;
  }
  return -1;
}

// ---- Session 67, part 5: the UI system's list widget (the PLAY / MATCH RULES / SELECT ARENA bar after the superstar
// select, the MATCH RULES rows; release run 4361: memory scan + write watch; coverage run 043: the list probe in
// probes\probes.cpp). A list (vtable 0x820660A8) steps its input in sub_82B49A70(list) while it has the focus: a press
// -> sub_82B49458(list, [list + 88] +- 1), the game's set-selected (stores the item at +88 through its +12 object and at
// +856; skips disabled items). +584 = the layout (0 = vertical, UP / DOWN; 1 = horizontal, LEFT / RIGHT), +612 = the
// item count. A list draws one layout node of its screen's tree: [[[list + 8] + 8] + 704] + [list + 576] * 272 ([list
// + 8] points into the stack: valid only while the step runs). The items' labels are text elements whose parent word
// (+16) is that node, their text first set in item order (bar: PLAY, MATCH RULES, SELECT ARENA; the bar's 11 variants
// with SAVE / CHANGE PARTICIPANTS / SIMULATE are 11 nodes; MATCH RULES: the four rows, then blank description lines),
// and their boxes (world centre +768 / +772, size +448 / +452) are where the items are (run 043: bar 298 / 640 / 982 x
// 598, 320 x 28; rows at y 234 + 24 n, 520 x 24 centred at x 468, the row band to x 1072). The set-text hook files each
// text element under its parent; the step hook takes the node's first `count` non-blank labels (only if they run along
// the list's axis), publishes their boxes, and makes the mouse's jump as for the SmartMenu lists: [list + 88] is put
// next to the target and one press lets the game's own step land there (its sound and highlight).
constexpr uint32_t kListIndexOffset = 88, kListOwnerOffset = 8, kListNodeIdOffset = 576, kListLayoutOffset = 584,
                   kListCountOffset = 612, kOwnerTreeOffset = 8, kTreeNodeCountOffset = 140, kTreeNodesOffset = 704,
                   kNodeStride = 272, kElemParentOffset = 16, kElemStringOffset = 1136;
constexpr int kMaxListItems = 24, kMaxUiLists = 4;

// The page header of a list's screen (MATCH RULES: "LB /// RB ENVIRONMENT"; run 4363, the text dump): the LB / RB
// button glyphs are text elements with the text ids 45004 / 45008 in another node of the same tree (MATCH RULES: node
// 3, the rows' pages are nodes 0-2), and each page is its own list whose node id is the page (ENVIRONMENT 0, the next
// two 1 and 2; RB on the last page does nothing). The page marks (three slashes) sit evenly between the two glyphs.
constexpr uint32_t kTextIdLb = 45004, kTextIdRb = 45008;
constexpr uint16_t kPadLb = 0x0100, kPadRb = 0x0200;
// Between two page presses of a page-mark jump: a tab press right after a tab change is dropped (run 4364, the arena:
// page 2 -> page 0 went RB-wise fine but stopped on page 1 going back; the second LB came 3 frames after the first).
// A UI-list screen's page-mark jump stays one LB / RB press per page: LB / RB on a page calls the dispatcher
// sub_82B55A78 with the NEIGHBOURING page's list (coverage run 044, trace: RB on page 1 -> page 2's list, which runs
// that page's enter, sub_82B17320; page 3's is sub_82B16108), and handing it page 3's list instead (run 4365) ran
// page 3's enter while the caller gave page 2's list the input: header RULES over page 2's rows, and leaving gave the
// game's "No win conditions have been set" notice. Not done again without the caller's focus change mapped.
constexpr uint64_t kPageSettleFrames = 12;
// The list's item records (sub_82B45CE8): 80 bytes each from [list + 760], [list + 764] of them; the halfword at +32,
// bit 0 = hidden.
constexpr uint32_t kListRecordsOffset = 760, kListRecordCountOffset = 764, kListRecordStride = 80;

// The match setup bar's button legend (Jarrod: "click these at the bottom"; 4361 capture measured, 1280 x 720): "D-pad
// ENTRANCE: ON" x 348-490 (the D-pad up / down icon: the bar itself only uses LEFT / RIGHT), "LT RT MATCH PACE:
// NORMAL" with LT x 520-531, RT 539-552, the words 559-728, all at y 645-668. OFF is wider than ON: the pace item
// moves right a little (the LT / RT split at 538 holds for most of it). LT / RT are triggers: pressed as the unused
// button bits below, turned into full trigger values by the pad filter.
constexpr uint16_t kPadLtTrigger = 0x0400, kPadRtTrigger = 0x0800;
constexpr float kLegendY0 = 645, kLegendY1 = 668, kLegendEntranceX0 = 348, kLegendEntranceX1 = 495,
                kLegendLtX0 = 512, kLegendSplitX = 538, kLegendPaceX1 = 750;

struct UiList {
  uint32_t list = 0, nodes = 0;
  bool legend = false;  // the match setup bar: its legend's ENTRANCE / MATCH PACE take clicks
  int layout = 0, count = 0, page = 0, pages = 0;  // pages > 1: the header below is set
  std::array<Box, kMaxListItems> items{}, values{};  // values: a row's value text (vertical lists), for its arrows
  Box lb{}, rb{};
  int64_t seen_ms = 0;
};

// What the pointer is on: an item (`after`: the press once there, A / LEFT / RIGHT), a page mark, or LB / RB.
struct UiHit {
  uint32_t list = 0, nodes = 0;
  int item = -1, page = -1;
  uint16_t after = 0, direct = 0;
};

struct UiLists {
  // Set-text (any thread): text elements by parent, in the order their text was first set.
  std::mutex children_mutex;
  std::unordered_map<uint32_t, std::vector<uint32_t>> children;
  std::unordered_map<uint32_t, uint32_t> text_id;  // element -> the text id its text was last set from
  size_t child_total = 0;
  // Game thread -> UI thread: the lists that stepped lately.
  std::mutex mutex;
  std::array<UiList, kMaxUiLists> lists{};
  std::atomic<int64_t> seen_ms{0};
  // UI -> game.
  std::atomic<uint32_t> target_list{0}, page_nodes{0}, direct_nodes{0};
  std::atomic<int> target{-1}, page_target{-1};
  std::atomic<uint16_t> after{0}, direct{0};
  uint32_t hover_list = 0;  // UI thread
  int hover = -1;
  // Game thread presses.
  uint64_t next_frame = 0, page_timeout = 0;
  std::atomic<uint64_t> release_frame{0};
  std::atomic<uint16_t> buttons{0};
  int active_target = -1, tries = 0, page_from = -1, page_tries = 0;
  uint32_t active_list = 0, logged_list = 0, logged_node = 0, refused_list = 0;
};

inline UiLists& UL() {
  static UiLists u;
  return u;
}

inline bool UiListsUp() { return UL().seen_ms.load() && NowMs() - UL().seen_ms.load() < 150; }

// A guest word our code may read (inside a heap, readable page; src/guest_read.h), else 0.
inline uint32_t SafeRead32(uint32_t a) { return a && !(a & 3) && GuestWordReadableFast(a) ? Read32(a) : 0; }

// Set-text hook, after the call: file the element under its parent.
inline void OnTextElement(uint32_t e, uint32_t id) {
  const uint32_t parent = SafeRead32(e + kElemParentOffset);
  if (!parent) return;
  UiLists& u = UL();
  std::lock_guard lock(u.children_mutex);
  if (u.child_total > 40000) {  // screens come and go: start over (the open screen's labels come back as they are set)
    u.children.clear();
    u.text_id.clear();
    u.child_total = 0;
  }
  u.text_id[e] = id;
  std::vector<uint32_t>& v = u.children[parent];
  if (std::find(v.begin(), v.end(), e) != v.end()) return;
  v.push_back(e);
  ++u.child_total;
}

// Debug (wwe2k14_mouse_menus_dump): every filed text element that still sits under its parent with a box on screen.
inline void DumpTextElements() {
  UiLists& u = UL();
  std::lock_guard lock(u.children_mutex);
  int n = 0;
  for (const auto& [parent, elements] : u.children)
    for (const uint32_t e : elements) {
      if (SafeRead32(e + kElemParentOffset) != parent || !SafeRead32(e + kElemX)) continue;
      const float x = ReadF(e + kElemX), y = ReadF(e + kElemY), w = ReadF(e + kElemW), h = ReadF(e + kElemH);
      if (!(w > 0 && h > 0 && x > -100 && x < kUiW + 100 && y > -100 && y < kUiH + 100)) continue;
      char text[48] = {};
      if (const uint32_t str = SafeRead32(e + kElemStringOffset))
        for (uint32_t i = 0; i + 1 < sizeof(text); ++i) {
          const char c = char(*Guest(str + i));
          if (!c) break;
          text[i] = (c >= 32 && c < 127) ? c : '?';
        }
      const auto id = u.text_id.find(e);
      REXLOG_INFO("[wwe2k14][mouse] dump: text {:08X} id {} '{}' centre ({:.0f}, {:.0f}) size {:.0f} x {:.0f}, parent "
                  "{:08X}",
                  e, id == u.text_id.end() ? 0u : id->second, text, x, y, w, h, parent);
      ++n;
    }
  REXLOG_INFO("[wwe2k14][mouse] dump: {} text elements on screen ({} filed)", n, u.child_total);
}

inline bool BlankText(uint32_t str) {
  for (uint32_t i = 0; i < 8; ++i) {
    const char c = char(*Guest(str + i));
    if (!c) return true;
    if (c != ' ') return false;
  }
  return false;
}

struct TextBox {
  Box box;
  float cx = 0, cy = 0;
};

// A filed text element still under `parent`, with text and a box: its box, else false.
inline bool LabelBox(uint32_t e, uint32_t parent, TextBox& t) {
  if (SafeRead32(e + kElemParentOffset) != parent || !SafeRead32(e + kElemX)) return false;
  const uint32_t str = SafeRead32(e + kElemStringOffset);
  if (!str || BlankText(str)) return false;
  const float x = ReadF(e + kElemX), y = ReadF(e + kElemY), w = ReadF(e + kElemW), h = ReadF(e + kElemH);
  if (!(w > 0 && h > 0 && w < 1280 && h < 200)) return false;
  t = {{x - w / 2, y - h / 2, x + w / 2, y + h / 2, true}, x, y};
  return true;
}

// Game thread, the list's input step (before it runs): publish the items' boxes (and the page header); then the next
// press toward what the mouse asked for.
inline void OnListStep(uint32_t list) {
  UiLists& u = UL();
  const int layout = int(SafeRead32(list + kListLayoutOffset)), count = int(SafeRead32(list + kListCountOffset));
  if ((layout != 0 && layout != 1) || count <= 0 || count > kMaxListItems) return;
  const uint32_t owner = SafeRead32(list + kListOwnerOffset), tree = SafeRead32(owner + kOwnerTreeOffset);
  const uint32_t nodes = SafeRead32(tree + kTreeNodesOffset), id = SafeRead32(list + kListNodeIdOffset);
  const uint32_t node_count = SafeRead32(tree + kTreeNodeCountOffset);
  if (!nodes || id >= node_count) return;
  const uint32_t node = nodes + id * kNodeStride;
  UiList l;
  l.list = list, l.nodes = nodes, l.layout = layout, l.count = count, l.page = int(id), l.seen_ms = NowMs();
  std::vector<TextBox> texts;
  {
    std::lock_guard lock(u.children_mutex);
    if (const auto it = u.children.find(node); it != u.children.end())
      for (const uint32_t e : it->second)
        if (TextBox t; texts.size() < 64 && LabelBox(e, node, t)) texts.push_back(t);
    // The page header: the LB / RB glyphs in any node of this tree.
    for (uint32_t j = 0; j < node_count && j < 32; ++j) {
      const uint32_t nd = nodes + j * kNodeStride;
      const auto it = u.children.find(nd);
      if (it == u.children.end()) continue;
      for (const uint32_t e : it->second) {
        const auto tid = u.text_id.find(e);
        if (tid == u.text_id.end() || (tid->second != kTextIdLb && tid->second != kTextIdRb)) continue;
        TextBox t;
        if (LabelBox(e, nd, t)) (tid->second == kTextIdLb ? l.lb : l.rb) = t.box;
      }
    }
  }
  if (l.lb.set && l.rb.set && l.lb.x1 < l.rb.x0 && std::fabs(l.lb.y0 - l.rb.y0) < 4 && node_count >= 3)
    l.pages = std::min(int(node_count) - 1, 8);  // the pages' nodes, then the header's
  // The items: the labels sharing one column (vertical) or one row (horizontal), the first such group of `count`
  // (the leftmost / topmost: a vertical list's values share a column too), in screen order.
  auto along = [layout](const TextBox& t) { return layout == 1 ? t.cx : t.cy; };
  auto across = [layout](const TextBox& t) { return layout == 1 ? t.cy : t.cx; };
  std::vector<TextBox> items;
  // Hidden items (records +32 bit 0; set-selected and the count skip them) keep their labels: MATCH RULES page 3
  // outside Extreme Rules has 6 items but 10 row labels, the four SLOT rows hidden (run 4364). Then a column of one
  // label per record is taken and the hidden records' labels dropped.
  const uint32_t records = SafeRead32(list + kListRecordsOffset);
  const int record_count = int(SafeRead32(list + kListRecordCountOffset));
  for (const int want : {count, record_count}) {
    float best = 1e9f;
    for (const TextBox& key : texts) {
      std::vector<TextBox> group;
      for (const TextBox& t : texts)
        if (std::fabs(across(t) - across(key)) <= 2) group.push_back(t);
      if (int(group.size()) == want && across(key) < best) best = across(key), items = std::move(group);
    }
    if (int(items.size()) != want) continue;
    if (want != count) {  // one label per record: drop the hidden ones
      std::sort(items.begin(), items.end(), [&](const TextBox& a, const TextBox& b) { return along(a) < along(b); });
      std::vector<TextBox> shown;
      for (int r = 0; r < want && records && want <= 64; ++r)
        if (!(SafeRead32(records + uint32_t(r) * kListRecordStride + 32) & 0x00010000u)) shown.push_back(items[r]);
      items = std::move(shown);
    }
    break;
  }
  if (int(items.size()) != count) {
    if (list != u.refused_list) {
      u.refused_list = list;
      REXLOG_INFO("[wwe2k14][mouse] UI list {:08X}: {} items but no column of {} labels in node {:08X} ({} texts): no "
                  "mouse there",
                  list, count, count, node, texts.size());
    }
    return;
  }
  std::sort(items.begin(), items.end(), [&](const TextBox& a, const TextBox& b) { return along(a) < along(b); });
  for (int i = 0; i < count; ++i) {  // each item's box reaches halfway to its neighbours; rows span their centred panel
    const TextBox& t = items[size_t(i)];
    Box b = t.box;
    if (layout == 1) {
      if (i > 0) b.x0 = (items[size_t(i - 1)].cx + t.cx) / 2;
      if (i + 1 < count) b.x1 = (t.cx + items[size_t(i + 1)].cx) / 2;
    } else {
      if (i > 0) b.y0 = (items[size_t(i - 1)].cy + t.cy) / 2;
      if (i + 1 < count) b.y1 = (t.cy + items[size_t(i + 1)].cy) / 2;
      b.x1 = std::max(b.x1, kUiW - b.x0);
      for (const TextBox& v : texts)  // the row's value (right of the label, same height): its arrows
        if (std::fabs(v.cy - t.cy) < 3 && v.box.x0 >= t.box.x1 - 2) l.values[size_t(i)] = v.box;
    }
    l.items[size_t(i)] = b;
  }
  // The match setup bar (across, 3-5 buttons on the row at y ~598): its legend takes clicks.
  l.legend = layout == 1 && count >= 3 && count <= 5 && std::fabs(items[0].cy - 598) < 10;
  {
    std::lock_guard lock(u.mutex);
    size_t slot = 0;
    for (size_t i = 0; i < u.lists.size(); ++i) {
      if (u.lists[i].list == list) {
        slot = i;
        break;
      }
      if (u.lists[i].seen_ms < u.lists[slot].seen_ms) slot = i;
    }
    u.lists[slot] = l;
  }
  u.seen_ms = l.seen_ms;
  if (list != u.logged_list || node != u.logged_node) {
    u.logged_list = list, u.logged_node = node;
    REXLOG_INFO("[wwe2k14][mouse] UI list {:08X} has the input: {} items {}, node {:08X}, first ({:.0f}, {:.0f}) - "
                "({:.0f}, {:.0f}); page {} of {}",
                list, count, layout ? "across" : "down", node, l.items[0].x0, l.items[0].y0, l.items[0].x1,
                l.items[0].y1, l.page + 1, l.pages);
  }
  // The mouse's next press.
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (frame < u.next_frame) return;
  auto press = [&](uint16_t b, const char* what) {
    if (REXCVAR_GET(wwe2k14_mouse_menus_log))
      REXLOG_INFO("[wwe2k14][mouse] UI list frame {}: press {:04X} ({}; item {}, page {}, list {:08X})", frame, b, what,
                  Read32(list + kListIndexOffset), id, list);
    u.buttons = b;
    u.release_frame = frame + kHoldFrames;
    u.next_frame = frame + kHoldFrames + 1;
  };
  if (g_input_script.Busy() || InMatch()) {
    u.target = -1, u.after = 0, u.page_target = -1, u.direct = 0;
    return;
  }
  if (u.direct_nodes.load() == nodes)  // LB / RB clicked
    if (const uint16_t d = u.direct.exchange(0)) return press(d, "header / legend button clicked");
  if (const int pt = u.page_target.load(); pt >= 0 && u.page_nodes.load() == nodes) {  // a page mark clicked
    if (pt == int(id) || pt >= l.pages) {
      u.page_target = -1;
    } else if (int(id) != u.page_from || frame >= u.page_timeout) {  // the last press took (or timed out): the next
      if (++u.page_tries > 8) {
        u.page_target = -1;
      } else {
        u.page_from = int(id);
        u.page_timeout = frame + 60;
        press(pt > int(id) ? kPadRb : kPadLb, "to the page clicked");
        u.next_frame = frame + kHoldFrames + kPageSettleFrames;
      }
    }
    return;
  }
  if (u.target_list.load() != list) return;
  const int target = u.target.load();
  if (target < 0) return;
  if (target >= count) {
    u.target = -1, u.after = 0;
    return;
  }
  if (target != u.active_target || list != u.active_list) u.active_target = target, u.active_list = list, u.tries = 0;
  const int index = int(Read32(list + kListIndexOffset));
  if (index == target) {
    int expected = target;
    u.target.compare_exchange_strong(expected, -1);
    if (const uint16_t a = u.after.exchange(0)) press(a, a == kPadA ? "pick" : "value arrow");
    return;
  }
  if (++u.tries > 4) {  // a disabled item (set-selected walks past it)
    u.target = -1, u.after = 0;
    return;
  }
  const bool forward = target > index;
  if (std::abs(target - index) > 1)  // the step adds +-1 to [list + 88]: start it next to the target
    rex::memory::store_and_swap<uint32_t>(Guest(list + kListIndexOffset), uint32_t(forward ? target - 1 : target + 1));
  press(layout == 1 ? (forward ? kPadRight : kPadLeft) : (forward ? kPadDown : kPadUp), "to the item");
}

inline void ListPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame) {
  UiLists& u = UL();
  if (const uint16_t add = u.buttons.load(); add && frame < u.release_frame && NowMs() - u.seen_ms.load() < 60) {
    if (add & kPadLtTrigger) state->gamepad.left_trigger = 255;
    if (add & kPadRtTrigger) state->gamepad.right_trigger = 255;
    state->gamepad.buttons = uint16_t(state->gamepad.buttons | (add & ~(kPadLtTrigger | kPadRtTrigger)));
  }
}

inline bool Inside(const Box& b, float x, float y, float pad = 0) {
  return b.set && x >= b.x0 - pad && x < b.x1 + pad && y >= b.y0 - pad && y < b.y1 + pad;
}

// UI thread: what of a UI list is under the game-screen point (false: nothing).
inline bool UiListHit(float ux, float uy, UiHit& h) {
  UiLists& u = UL();
  const int64_t now = NowMs();
  std::lock_guard lock(u.mutex);
  for (const UiList& l : u.lists) {
    if (!l.list || now - l.seen_ms > 150) continue;
    h = {};
    h.list = l.list, h.nodes = l.nodes;
    if (l.legend && uy >= kLegendY0 && uy < kLegendY1) {  // the bar's legend: ENTRANCE, MATCH PACE (LT / RT)
      if (ux >= kLegendEntranceX0 && ux < kLegendEntranceX1) return h.direct = kPadDown, true;
      if (ux >= kLegendLtX0 && ux < kLegendSplitX) return h.direct = kPadLtTrigger, true;
      if (ux >= kLegendSplitX && ux < kLegendPaceX1) return h.direct = kPadRtTrigger, true;
    }
    if (l.pages > 1) {  // the header: LB, RB, and the page marks between them
      if (Inside(l.lb, ux, uy, 6)) return h.direct = kPadLb, true;
      if (Inside(l.rb, ux, uy, 6)) return h.direct = kPadRb, true;
      if (ux >= l.lb.x1 && ux < l.rb.x0 && uy >= l.lb.y0 - 4 && uy < l.lb.y1 + 4) {
        h.page = std::clamp(int((ux - l.lb.x1) * float(l.pages) / (l.rb.x0 - l.lb.x1)), 0, l.pages - 1);
        return true;
      }
    }
    for (int i = 0; i < l.count; ++i) {
      const Box& b = l.items[size_t(i)];
      if (!(uy >= b.y0 && uy < b.y1)) continue;
      if (const Box& v = l.values[size_t(i)]; v.set) {  // the value's arrows (drawn on the highlighted row)
        if (ux >= v.x0 - 28 && ux < v.x0 + 12) return h.item = i, h.after = kPadLeft, true;
        if (ux >= v.x1 - 12 && ux < v.x1 + 28) return h.item = i, h.after = kPadRight, true;
      }
      // A row with a value: a click on the row steps the value (RIGHT), not A: A on a MATCH RULES row closes the
      // screen (Jarrod, run 4370: "clicking text trigger a right input ... avoids accidentally closing the screen").
      if (ux >= b.x0 && ux < b.x1) return h.item = i, h.after = l.values[size_t(i)].set ? kPadRight : kPadA, true;
    }
  }
  return false;
}

// ---- Session 67, part 6: the SELECT ARENA tiles (release run 4362: memory scan, write watch in coverage run 043,
// a snapshot of the screen object). The arena screen is one of the shared grid widgets (vtable 0x82039240): the base
// input sub_823BC8C0(W) reads the presses (+56), moves the cursor in sub_823BC968 (row +20, 4 rows at +24; column +36,
// 5 at +40, both wrapping) and calls the widget's vtable +148 every frame, here sub_8286BD68(W) (copies row / column
// to +13540 / +13544; hooked: the arena grid has the input). The column is the tile's place in its row (Royal Rumble
// = row 1, column 2). The tiles are records of 480 bytes from W + 4236, each with a sprite at +184 (vtable
// 0x8206A7B8) whose rectangle is at +408 / +412 (x0, y0) and +424 / +428 (x1, y1) in a 640 x 360 screen (twice that on
// the 1280 x 720 one; ARENAS: 5 / 4 / 4 / 5 tiles, records 0-17, record 18 empty). Rows and columns come from the
// rectangles (rows by height, columns left to right). The jump: the cursor is put next to the target in its row and
// one LEFT / RIGHT press lets the game's own move land there.
constexpr uint32_t kArenaVtable = 0x82039240, kArenaTileBase = 4236, kArenaTileStride = 480, kArenaSpriteOffset = 184,
                   kArenaSpriteVtable = 0x8206A7B8, kArenaRectX0 = 408, kArenaRectY0 = 412, kArenaRectX1 = 424,
                   kArenaRectY1 = 428, kGridWRowOffset = 20, kGridWColOffset = 36;
constexpr int kMaxArenaTiles = 32;
// The grids whose tiles are such sprite records (rect at sprite + 224 / 228 / 240 / 244): the ARENAS tab above, and
// the WRESTLEMANIA ARENAS tab's own grid widget (vtable 0x820394C0, run 4366: memory scan + write watch + snapshot;
// vtable +148 sub_8286D958 copies row / column to +348 / +352, +96 sub_8286F510 scrolls on UP / DOWN past the
// visible rows; 5 columns, 4 visible rows; sprites from +388, the first five records empty: a row kept off screen).
// The custom arenas tab has none on Jarrod's save to look at: no mouse there yet.
// Scrolling (sub_8286F510): DOWN on the last visible row adds 1 to the scroll (+252) up to +256 (the rows in all: 6,
// 28 arenas) - +24 (the rows shown: 4), UP on row 0 takes 1 off down to 0; the wheel scrolls through those presses.
// CUSTOM ARENAS (run 4368, Jarrod's save: 10 rows of EMPTY slots, all navigable): its own grid widget (vtable
// 0x82039390; +148 sub_8286CC68 copies to +1072 / +1076; +96 sub_82870568 scrolls the same way, +256 = 10) without
// sprite records of its own: its tiles are the WRESTLEMANIA layout, measured on screen (first_sprite 0: 5 columns, x
// 164 + 192 col, 184 wide; y 126 + 120 row, 112 tall).
struct SpriteGridClass {
  uint32_t vtable, first_sprite, copy_row, copy_col, scroll;  // first_sprite 0: the measured 5-column layout; scroll 0:
};                                                           // no scrolling
constexpr SpriteGridClass kArenaGrid = {kArenaVtable, kArenaTileBase + kArenaSpriteOffset, 13540, 13544, 0};
constexpr SpriteGridClass kWmArenaGrid = {0x820394C0, 388, 348, 352, 252};
constexpr SpriteGridClass kCustomArenaGrid = {0x82039390, 0, 1072, 1076, 252};
constexpr uint32_t kGridRowsShownOffset = 24, kGridRowsAllOffset = 256;
// The tiles shown (1280 x 720): rows of records above / below the panel (kept for scrolling) are not hovered (run
// 4367: a row sat under the bottom bar at y ~612).
constexpr float kArenaPanelY0 = 110, kArenaPanelY1 = 615;

struct ArenaTile {
  Box box;
  int row = 0, col = 0;
};

struct Arena {
  std::atomic<int64_t> seen_ms{0};
  std::mutex mutex;  // guards tiles / count (game thread -> UI thread)
  std::array<ArenaTile, kMaxArenaTiles> tiles{};
  int count = 0;
  // UI -> game.
  std::atomic<int> target_row{-1}, target_col{-1};
  std::atomic<bool> select{false};
  std::atomic<int> wheel{0};  // + = scroll down (rows), - = up
  int hover_row = -2, hover_col = -2;  // UI thread
  // Game thread presses.
  uint64_t next_frame = 0;
  std::atomic<uint64_t> release_frame{0};
  std::atomic<uint16_t> buttons{0};
  int tries = 0, active_row = -1, active_col = -1;
  int logged_count = -1;
  // The jump's start next to the target, set inside the game's move (sub_823BC968 hook) right before it runs: set
  // earlier, the screen's per-frame copy drew the highlight there for a frame (Jarrod, run 4363: "the option to the left
  // of the selection hovers" for a split second).
  bool jump_pending = false;
  uint32_t jump_w = 0;
  int jump_row = 0, jump_col = 0, jump_target_row = 0, jump_target_col = 0;
  uint32_t jump_copy_row = 0, jump_copy_col = 0;  // the grid class's copy of the position (its remap / scroll reads it)
  uint64_t jump_until = 0;
  // The header (LB /// RB): seen by the grid's per-frame update even on a tab with nothing to pick; clicks.
  std::atomic<int64_t> header_ms{0};
  std::atomic<uint16_t> direct{0};  // LB / RB clicked
  std::atomic<int> page_target{-1};  // a page mark clicked (0-2)
  uint64_t header_frame = 0, page_timeout = 0;
  int page_from = -1, page_tries = 0;
  // (A one-press page jump, starting sub_8286F280's tab step from the slot next to the target, never fired in run
  // 4366: its hook saw no call with the widget. Pages go one LB / RB at a time; Jarrod: a direct jump later, natively.)
};

inline Arena& AR() {
  static Arena a;
  return a;
}

inline bool ArenaUp() { return AR().seen_ms.load() && NowMs() - AR().seen_ms.load() < 150; }

// Game thread, every frame the arena grid has the input (after its copy): the tiles, then the next press.
inline void OnArenaHeader(uint32_t w);
inline void OnArenaFrame(uint32_t w, const SpriteGridClass& c) {
  Arena& a = AR();
  if (c.vtable == kArenaVtable) OnArenaHeader(w);  // (the header also from the update hook, on every tab)
  std::array<ArenaTile, kMaxArenaTiles> tiles{};
  int n = 0;
  if (!c.first_sprite) {  // the measured layout: every slot of the rows shown
    const int rows = std::clamp(int(Read32(w + kGridRowsShownOffset)), 0, 4);
    for (int r = 0; r < rows; ++r)
      for (int col = 0; col < 5; ++col) {
        const float x0 = 164.0f + 192.0f * float(col), y0 = 126.0f + 120.0f * float(r);
        tiles[size_t(n++)].box = {x0, y0, x0 + 184, y0 + 112, true};
      }
  }
  for (int k = 0; c.first_sprite && k < kMaxArenaTiles; ++k) {
    const uint32_t sprite = w + c.first_sprite + uint32_t(k) * kArenaTileStride;
    if (SafeRead32(sprite) != kArenaSpriteVtable) break;
    const float x0 = 2 * ReadF(sprite + kArenaRectX0 - kArenaSpriteOffset),
                y0 = 2 * ReadF(sprite + kArenaRectY0 - kArenaSpriteOffset),
                x1 = 2 * ReadF(sprite + kArenaRectX1 - kArenaSpriteOffset),
                y1 = 2 * ReadF(sprite + kArenaRectY1 - kArenaSpriteOffset);
    if (!(x1 > x0 && y1 > y0 && x1 - x0 < 640 && y1 - y0 < 360)) continue;  // an empty record
    if (y0 < kArenaPanelY0 || y1 > kArenaPanelY1) continue;                   // a row kept off the panel
    tiles[size_t(n++)].box = {x0, y0, x1, y1, true};
  }
  // Rows by height (a new row when the top moves by more than half a tile), columns left to right in each row.
  std::sort(tiles.begin(), tiles.begin() + n, [](const ArenaTile& p, const ArenaTile& q) {
    return std::fabs(p.box.y0 - q.box.y0) > 20 ? p.box.y0 < q.box.y0 : p.box.x0 < q.box.x0;
  });
  int row = 0, col = 0;
  for (int i = 0; i < n; ++i) {
    if (i > 0 && std::fabs(tiles[size_t(i)].box.y0 - tiles[size_t(i - 1)].box.y0) > 20) ++row, col = 0;
    tiles[size_t(i)].row = row, tiles[size_t(i)].col = col++;
  }
  {
    std::lock_guard lock(a.mutex);
    a.tiles = tiles;
    a.count = n;
  }
  if (!ArenaUp() || n != a.logged_count) {
    a.logged_count = n;
    REXLOG_INFO("[wwe2k14][mouse] arena tiles have the input: {} tiles in {} rows (widget {:08X}, row {}, column {})",
                n, n ? row + 1 : 0, w, Read32(w + kGridWRowOffset), Read32(w + kGridWColOffset));
  }
  a.seen_ms = NowMs();
  // The mouse's next press.
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (frame < a.next_frame) return;
  // The wheel (a scrolling grid): onto the bottom / top shown row first (the jump below), then DOWN / UP scrolls.
  if (const int wheel = a.wheel.load(); wheel && a.target_row.load() < 0) {
    const int dir = wheel > 0 ? 1 : -1, scroll = c.scroll ? int(Read32(w + c.scroll)) : 0;
    const int scroll_max = int(Read32(w + kGridRowsAllOffset)) - int(Read32(w + kGridRowsShownOffset));
    if (!c.scroll || n == 0 || (dir > 0 ? scroll >= scroll_max : scroll <= 0) || g_input_script.Busy() || InMatch()) {
      a.wheel = 0;  // nothing (more) to scroll
    } else if (const int edge = dir > 0 ? row : 0, cr = int(Read32(w + kGridWRowOffset)); cr != edge) {
      int len = 0;
      for (int i = 0; i < n; ++i) len += tiles[size_t(i)].row == edge;
      a.target_col = std::clamp(int(Read32(w + kGridWColOffset)), 0, std::max(len - 1, 0));
      a.target_row = edge;
    } else {
      a.wheel -= dir;
      if (REXCVAR_GET(wwe2k14_mouse_menus_log))
        REXLOG_INFO("[wwe2k14][mouse] arena frame {}: wheel press {} (scroll {})", frame, dir > 0 ? "DOWN" : "UP",
                    scroll);
      a.buttons = dir > 0 ? kPadDown : kPadUp;
      a.release_frame = frame + kHoldFrames;
      a.next_frame = frame + kHoldFrames + 1;
      return;
    }
  }
  const int tr = a.target_row.load(), tc = a.target_col.load();
  if (tr < 0 || tc < 0) return;
  int row_len = 0;
  for (int i = 0; i < n; ++i) row_len += tiles[size_t(i)].row == tr;
  if (tc >= row_len || g_input_script.Busy() || InMatch()) {
    a.target_row = -1, a.target_col = -1, a.select = false;
    return;
  }
  if (tr != a.active_row || tc != a.active_col) a.active_row = tr, a.active_col = tc, a.tries = 0;
  const int cr = int(Read32(w + kGridWRowOffset)), cc = int(Read32(w + kGridWColOffset));
  uint16_t press = 0;
  if (cr == tr && cc == tc) {
    a.target_row = -1, a.target_col = -1;
    if (a.select.exchange(false)) press = kPadA;
  } else if (++a.tries > 3 || row_len < 2) {
    a.target_row = -1, a.target_col = -1, a.select = false;
    return;
  } else {  // next to the target in its row (set when the move runs), then one press onto it
    const bool from_left = tc > 0;
    a.jump_pending = true, a.jump_w = w, a.jump_row = tr, a.jump_col = from_left ? tc - 1 : tc + 1;
    a.jump_target_row = tr, a.jump_target_col = tc;
    a.jump_copy_row = c.copy_row, a.jump_copy_col = c.copy_col;
    a.jump_until = frame + kHoldFrames + 3;
    press = from_left ? kPadRight : kPadLeft;
  }
  if (!press) return;
  if (REXCVAR_GET(wwe2k14_mouse_menus_log))
    REXLOG_INFO("[wwe2k14][mouse] arena frame {}: press {:04X} (at row {} column {}, target row {} column {})", frame,
                press, cr, cc, tr, tc);
  a.buttons = press;
  a.release_frame = frame + kHoldFrames;
  a.next_frame = frame + kHoldFrames + 1;
}

// The arena screen's header (run 4363 screenshot, 1280 x 720): LB at x 286-316, RB at 430-460, the three page marks
// (slashes) at 348-395, all around y 96. The tab is W + 4224: 0, 1, 4 for the three tabs (ARENAS, WRESTLEMANIA
// ARENAS, the third; run 4363 snapshot over RB, RB, LB, LB on Jarrod's frozen save: 0 1 4 1 0; other values: no
// page-mark clicks, LB / RB still work).
constexpr Box kArenaLb = {282, 84, 320, 108, true}, kArenaRb = {426, 84, 464, 108, true};
constexpr float kArenaMarksX0 = 345, kArenaMarksX1 = 398, kArenaMarksY0 = 84, kArenaMarksY1 = 108;
constexpr uint32_t kArenaTabOffset = 4224;
constexpr int kArenaTabs[] = {0, 1, 4};

inline bool ArenaHeaderUp() { return AR().header_ms.load() && NowMs() - AR().header_ms.load() < 150; }

inline int ArenaTabIndex(uint32_t w) {
  const int v = int(Read32(w + kArenaTabOffset));
  for (int i = 0; i < 3; ++i)
    if (kArenaTabs[i] == v) return i;
  return -1;
}

// Game thread, once a frame while the arena screen updates (any tab): the header's clicks.
inline void OnArenaHeader(uint32_t w) {
  Arena& a = AR();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (frame == a.header_frame) return;
  if (!ArenaHeaderUp()) REXLOG_INFO("[wwe2k14][mouse] arena header is up (widget {:08X}, tab {})", w, ArenaTabIndex(w));
  a.header_frame = frame;
  a.header_ms = NowMs();
  if (frame < a.next_frame) return;
  auto press = [&](uint16_t b, const char* what) {
    if (REXCVAR_GET(wwe2k14_mouse_menus_log))
      REXLOG_INFO("[wwe2k14][mouse] arena frame {}: press {:04X} ({}, tab {})", frame, b, what, ArenaTabIndex(w));
    a.buttons = b;
    a.release_frame = frame + kHoldFrames;
    a.next_frame = frame + kHoldFrames + 1;
  };
  if (g_input_script.Busy() || InMatch()) {
    a.direct = 0, a.page_target = -1;
    return;
  }
  if (const uint16_t d = a.direct.exchange(0)) return press(d, "LB / RB clicked");
  const int pt = a.page_target.load(), cur = ArenaTabIndex(w);
  if (pt < 0) return;
  if (cur < 0 || pt == cur) {
    a.page_target = -1;
  } else if (cur != a.page_from || frame >= a.page_timeout) {  // the last press took (or timed out): the next
    if (++a.page_tries > 6) {
      a.page_target = -1;
      return;
    }
    a.page_from = cur;
    a.page_timeout = frame + 60;
    press(pt > cur ? kPadRb : kPadLb, "to the page clicked");
    a.next_frame = frame + kHoldFrames + kPageSettleFrames;
  }
}

// UI thread: a click on the arena header: true if it was one (then handled).
inline bool ArenaHeaderClick(float ux, float uy) {
  Arena& a = AR();
  if (Inside(kArenaLb, ux, uy)) {
    a.direct = kPadLb;
  } else if (Inside(kArenaRb, ux, uy)) {
    a.direct = kPadRb;
  } else if (ux >= kArenaMarksX0 && ux < kArenaMarksX1 && uy >= kArenaMarksY0 && uy < kArenaMarksY1) {
    a.page_tries = 0, a.page_from = -1, a.page_timeout = 0;
    a.page_target = std::clamp(int((ux - kArenaMarksX0) * 3 / (kArenaMarksX1 - kArenaMarksX0)), 0, 2);
  } else {
    return false;
  }
  if (REXCVAR_GET(wwe2k14_mouse_menus_log))
    REXLOG_INFO("[wwe2k14][mouse] arena header click ({:.0f}, {:.0f}): button {:04X}, page {}", ux, uy, a.direct.load(),
                a.page_target.load());
  return true;
}

inline void ArenaPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame) {
  Arena& a = AR();
  const int64_t seen = std::max(a.seen_ms.load(), a.header_ms.load());
  if (const uint16_t add = a.buttons.load(); add && frame < a.release_frame && NowMs() - seen < 60)
    state->gamepad.buttons = uint16_t(state->gamepad.buttons | add);
}

// UI thread: the tile under the game-screen point (its row and column), or false.
inline bool ArenaTileAt(float ux, float uy, int& row, int& col) {
  Arena& a = AR();
  std::lock_guard lock(a.mutex);
  for (int i = 0; i < a.count; ++i) {
    const ArenaTile& t = a.tiles[size_t(i)];
    if (ux >= t.box.x0 && ux < t.box.x1 && uy >= t.box.y0 && uy < t.box.y1) {
      row = t.row, col = t.col;
      return true;
    }
  }
  return false;
}

// ---- Session 67, part 7: the button legend's A and B (Jarrod: "the A for Select and the B for back are pressable, just
// like the legend for pace and entrances"). The legend (bottom left, y 645-668) fetches its button icons through the text
// manager from one call site (return address 0x8293CA74; text ids 45000 = A, 45001 = B, 45023 = the D-pad up / down,
// 262 = LT RT; the labels from 0x8293CB0C) each time it is built, in screen order (MATCH RULES: A, B; the setup bar: A,
// B, D-pad, LT RT; run 4361 text probe). A click on its first item presses A only while the last legend built starts
// with A, on its second item B only while that is B (SELECT x 138-230; BACK / CANCEL 250-345; measured, 4361 capture).
constexpr uint32_t kLegendIconCaller = 0x8293CA74, kLegendIconA = 45000, kLegendIconB = 45001;
constexpr float kLegendAX0 = 138, kLegendAX1 = 230, kLegendBX0 = 250, kLegendBX1 = 345;

struct LegendIcons {
  std::mutex mutex;
  std::array<uint32_t, 8> icons{};
  int count = 0;
  uint64_t frame = 0;
  // A click (UI thread -> the pad filter).
  std::atomic<uint16_t> button{0};
  std::atomic<uint64_t> until{0};
};

inline LegendIcons& LG() {
  static LegendIcons l;
  return l;
}

// The GetString hook (any thread): a legend icon; a new frame starts a new legend.
inline void OnTextFetch(uint32_t id, uint32_t lr) {
  if (lr != kLegendIconCaller) return;
  LegendIcons& l = LG();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  std::lock_guard lock(l.mutex);
  if (frame != l.frame) {
    l.count = 0, l.frame = frame;
    if (REXCVAR_GET(wwe2k14_mouse_menus_log))  // which screens build their legend here (sequence: the mode)
      REXLOG_INFO("[wwe2k14][mouse] legend built (frame {}, sequence {}), first icon {}", frame, SequenceId(), id);
  }
  if (l.count < int(l.icons.size())) l.icons[size_t(l.count++)] = id;
}

// UI thread: a click on the legend's A / B item: true if it was one (then pressed).
inline bool LegendClick(float ux, float uy) {
  if (uy < kLegendY0 || uy >= kLegendY1) return false;
  LegendIcons& l = LG();
  uint16_t button = 0;
  {
    std::lock_guard lock(l.mutex);
    if (ux >= kLegendAX0 && ux < kLegendAX1 && l.count >= 1 && l.icons[0] == kLegendIconA) button = kPadA;
    if (ux >= kLegendBX0 && ux < kLegendBX1 && l.count >= 2 && l.icons[1] == kLegendIconB) button = kPadB;
  }
  if (!button) return false;
  l.button = button;
  l.until = g_guest_frame.load(std::memory_order_relaxed) + 2;  // seen by the next frame's input poll
  if (REXCVAR_GET(wwe2k14_mouse_menus_log))
    REXLOG_INFO("[wwe2k14][mouse] legend click ({:.0f}, {:.0f}): {}", ux, uy, button == kPadA ? "A" : "B");
  return true;
}

inline void LegendPadFilter(rex::input::X_INPUT_STATE* state, uint64_t frame) {
  LegendIcons& l = LG();
  if (frame < l.until.load()) state->gamepad.buttons = uint16_t(state->gamepad.buttons | l.button.load());
}

class Listener final : public rex::ui::WindowInputListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!REXCVAR_GET(wwe2k14_mouse_menus) || MouseTaken()) return;
    State& s = S();
    // The slot handler keeps running while the portrait grid is open (run 4362: no grid hover after a slot click):
    // the grid goes first.
    if (SlotsUp() && !GridUp() && SmartMenu()) {  // the player slots: a move onto another slot steps there
      float ux = 0, uy = 0;
      const int slot = ToUi(e.x(), e.y(), ux, uy) ? SlotAt(ux, uy) : -1;
      Slots& sl = SL();
      if (slot != sl.hover) {
        sl.hover = slot;
        if (slot >= 0) sl.target = slot;
        if (REXCVAR_GET(wwe2k14_mouse_menus_log) && slot >= 0)
          REXLOG_INFO("[wwe2k14][mouse] slots pointer game ({:.0f}, {:.0f}): slot {}", ux, uy, slot);
      }
      return;
    }
    if (GridUp() && SmartMenu()) {  // the superstar grid: a move onto another cell jumps there
      float ux = 0, uy = 0;
      int col = -1, row = -1;
      if (!ToUi(e.x(), e.y(), ux, uy) || !CellAt(ux, uy, col, row)) col = row = -1;
      Grid& g = G();
      if (col != g.hover_col || row != g.hover_row) {
        g.hover_col = col, g.hover_row = row;
        if (col >= 0) g.target_row = row, g.target_col = col;
        if (REXCVAR_GET(wwe2k14_mouse_menus_log) && col >= 0)
          REXLOG_INFO("[wwe2k14][mouse] grid pointer game ({:.0f}, {:.0f}): column {} row {}", ux, uy, col, row);
      }
      return;
    }
    if (ArenaUp() && SmartMenu()) {  // the SELECT ARENA tiles: a move onto another tile jumps there
      float ux = 0, uy = 0;
      int row = -1, col = -1;
      if (!ToUi(e.x(), e.y(), ux, uy) || !ArenaTileAt(ux, uy, row, col)) row = col = -1;
      Arena& a = AR();
      if (row != a.hover_row || col != a.hover_col) {
        a.hover_row = row, a.hover_col = col;
        if (row >= 0) a.target_row = row, a.target_col = col;
        if (REXCVAR_GET(wwe2k14_mouse_menus_log) && row >= 0)
          REXLOG_INFO("[wwe2k14][mouse] arena pointer game ({:.0f}, {:.0f}): row {} column {}", ux, uy, row, col);
      }
      return;
    }
    if (UiListsUp() && SmartMenu()) {  // a UI list (match setup bar, MATCH RULES rows): a move onto another item
      float ux = 0, uy = 0;
      UiHit h;
      if (!ToUi(e.x(), e.y(), ux, uy) || !UiListHit(ux, uy, h)) h = {};
      UiLists& u = UL();
      if (h.item != u.hover || h.list != u.hover_list) {
        u.hover = h.item, u.hover_list = h.list;
        if (h.item >= 0) u.target_list = h.list, u.target = h.item;
        if (REXCVAR_GET(wwe2k14_mouse_menus_log) && h.item >= 0)
          REXLOG_INFO("[wwe2k14][mouse] UI list pointer game ({:.0f}, {:.0f}): item {} of {:08X}", ux, uy, h.item,
                      h.list);
      }
      return;
    }
    uint32_t menu = 0;
    const int item = ItemAt(e.x(), e.y(), menu);
    if (item == s.hover_item && menu == s.hover_menu) return;  // only a move onto another item moves the cursor
    if (REXCVAR_GET(wwe2k14_mouse_menus_log)) {
      float ux = -1, uy = -1;
      ToUi(e.x(), e.y(), ux, uy);
      REXLOG_INFO("[wwe2k14][mouse] pointer ({}, {}) = game ({:.0f}, {:.0f}): item {} of list {} (list up: {})", e.x(),
                  e.y(), ux, uy, item, menu, ListUp());
    }
    s.hover_item = item, s.hover_menu = menu;
    if (item >= 0) s.target = item;
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (!REXCVAR_GET(wwe2k14_mouse_menus) || MouseTaken()) return;
    // Right click = B (Jarrod: "the right click emulate a b-press so you can go back and forth"): on every screen
    // outside a match (MouseTaken: CineCam's mouse look uses the right button there). Also where the main-menu system
    // is gone (Create a Superstar, 30 Years of WrestleMania; Jarrod: "so you can back out").
    if (e.button() == rex::ui::MouseEvent::Button::kRight) {
      if (RightButtonBound()) return;
      S().back_until = g_guest_frame.load(std::memory_order_relaxed) + 2;  // seen by the next frame's input poll
      if (REXCVAR_GET(wwe2k14_mouse_menus_log)) REXLOG_INFO("[wwe2k14][mouse] right click: B (sequence {})", SequenceId());
      return;
    }
    if (e.button() != rex::ui::MouseEvent::Button::kLeft) return;
    {  // the button legend's A / B (bottom left), on every screen outside a match
      float ux = 0, uy = 0;
      if (ToUi(e.x(), e.y(), ux, uy) && LegendClick(ux, uy)) return;
    }
    if (SlotsUp() && !GridUp() && SmartMenu()) {  // the player slots: step there and pick it (A)
      float ux = 0, uy = 0;
      const int slot = ToUi(e.x(), e.y(), ux, uy) ? SlotAt(ux, uy) : -1;
      if (slot < 0) return;
      Slots& sl = SL();
      sl.hover = slot;
      sl.target = slot;
      sl.select = true;
      if (REXCVAR_GET(wwe2k14_mouse_menus_log)) REXLOG_INFO("[wwe2k14][mouse] slots click: slot {}", slot);
      return;
    }
    if (GridUp() && SmartMenu()) {  // the superstar grid: jump to the cell and pick it (A)
      float ux = 0, uy = 0;
      int col = -1, row = -1;
      if (!ToUi(e.x(), e.y(), ux, uy) || !CellAt(ux, uy, col, row)) return;
      Grid& g = G();
      g.hover_col = col, g.hover_row = row;
      g.target_row = row, g.target_col = col;
      g.select = true;
      if (REXCVAR_GET(wwe2k14_mouse_menus_log))
        REXLOG_INFO("[wwe2k14][mouse] grid click: column {} row {}", col, row);
      return;
    }
    if (ArenaHeaderUp() && SmartMenu()) {  // the SELECT ARENA header: LB / RB, a page mark
      float ux = 0, uy = 0;
      if (ToUi(e.x(), e.y(), ux, uy) && ArenaHeaderClick(ux, uy)) return;
    }
    if (ArenaUp() && SmartMenu()) {  // the SELECT ARENA tiles: jump to the tile and pick it (A)
      float ux = 0, uy = 0;
      int row = -1, col = -1;
      if (!ToUi(e.x(), e.y(), ux, uy) || !ArenaTileAt(ux, uy, row, col)) return;
      Arena& a = AR();
      a.hover_row = row, a.hover_col = col;
      a.target_row = row, a.target_col = col;
      a.select = true;
      if (REXCVAR_GET(wwe2k14_mouse_menus_log))
        REXLOG_INFO("[wwe2k14][mouse] arena click: row {} column {}", row, col);
      return;
    }
    if (UiListsUp() && SmartMenu()) {  // a UI list: the item (A, or a value arrow), a page mark, LB / RB
      float ux = 0, uy = 0;
      UiHit h;
      if (!ToUi(e.x(), e.y(), ux, uy) || !UiListHit(ux, uy, h)) return;
      UiLists& u = UL();
      if (h.direct) {
        u.direct_nodes = h.nodes, u.direct = h.direct;
      } else if (h.page >= 0) {
        u.page_tries = 0, u.page_from = -1, u.page_timeout = 0;
        u.page_nodes = h.nodes, u.page_target = h.page;
      } else {
        u.hover = h.item, u.hover_list = h.list;
        u.after = h.after;
        u.target_list = h.list, u.target = h.item;
      }
      if (REXCVAR_GET(wwe2k14_mouse_menus_log))
        REXLOG_INFO("[wwe2k14][mouse] UI list click: item {} (then {:04X}), page {}, button {:04X} of {:08X}", h.item,
                    h.after, h.page, h.direct, h.list);
      return;
    }
    uint32_t menu = 0;
    const int item = ItemAt(e.x(), e.y(), menu);
    if (REXCVAR_GET(wwe2k14_mouse_menus_log))
      REXLOG_INFO("[wwe2k14][mouse] click at ({}, {}): item {} of list {}", e.x(), e.y(), item, menu);
    if (item < 0) return;
    State& s = S();
    s.hover_item = item, s.hover_menu = menu;
    s.target = item;
    s.select = true;
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!REXCVAR_GET(wwe2k14_mouse_menus) || MouseTaken()) return;
    const int detents = -e.scroll_y() / int(rex::ui::MouseEvent::kScrollPerDetent);  // wheel down = DOWN
    if (ArenaUp() && SmartMenu()) {  // a scrolling arena grid (WRESTLEMANIA ARENAS): one row per notch
      if (detents) AR().wheel = std::clamp(AR().wheel.load() + detents, -4, 4);
      return;
    }
    if (!ListUp() || !ListActive(SmartMenu())) return;
    if (REXCVAR_GET(wwe2k14_mouse_menus_log)) REXLOG_INFO("[wwe2k14][mouse] wheel {} ({} detents)", e.scroll_y(), detents);
    if (detents) S().wheel.fetch_add(std::clamp(detents, -4, 4));
  }
};

inline Listener& L() {
  static Listener l;
  return l;
}

// From ReXApp::OnCreateDialogs (UI thread).
inline void Install(rex::ui::Window* window) {
  if (!window) return;
  S().window = window;
  window->AddInputListener(&L(), 10);  // below the ImGui overlays (64): they take the mouse first when open
  g_mouse_pad_filter = PadFilter;
  g_text_fetch_observer = OnTextFetch;
  REXLOG_INFO("[wwe2k14][mouse] list menus follow the mouse: {}", REXCVAR_GET(wwe2k14_mouse_menus) ? "on" : "off");
}

}  // namespace wwe2k14::mouse_menu

REX_EXTERN(__imp__sub_8283B118);
REX_EXTERN(__imp__sub_82B53068);
// A list row (see the header): which slot, and (from the set-text call inside it) where its box is. Only calls that
// reach the set-text count (run 4340: other calls that set nothing came several times a frame and cut the frames short).
REX_HOOK_RAW(sub_8283B118) {
  using namespace wwe2k14::mouse_menu;
  if (!REXCVAR_GET(wwe2k14_mouse_menus)) return __imp__sub_8283B118(ctx, base);
  const int slot = int(ctx.r4.s32);
  const uint32_t rows = ctx.r3.u32;
  t_row = {};
  t_in_row = slot >= 0 && slot < kMaxSlots;
  __imp__sub_8283B118(ctx, base);
  t_in_row = false;
  if (!t_row.set) return;  // nothing set: not a row of the open list
  State& s = S();
  if (const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed); frame != s.build_frame) {
    OnMenuFrame(frame);  // the first row of a new frame
    s.build_frame = frame;
  }
  s.slot_count = rows ? int(Read32(rows + 4)) : 0;
  s.building[size_t(slot)] = t_row;
}
// A text element's set-text (every text in the game): inside a list row, that row's element and its box.
REX_HOOK_RAW(sub_82B53068) {
  using namespace wwe2k14::mouse_menu;
  if (t_in_row) {
    const uint32_t e = ctx.r3.u32;
    const float x = ReadF(e + kElemX), y = ReadF(e + kElemY), w = ReadF(e + kElemW), h = ReadF(e + kElemH);
    // (a row is ~296 x 24; while a list comes back from a screen its first frame can have a 1280-wide box: run 4344)
    if (w > 0 && h > 0 && w < 700 && h < 100) t_row = {x - w / 2, y - h / 2, x + w / 2, y + h / 2, true};
  }
  const uint32_t element = ctx.r3.u32, id = ctx.r4.u32;
  __imp__sub_82B53068(ctx, base);
  if (REXCVAR_GET(wwe2k14_mouse_menus)) OnTextElement(element, id);  // filed under its parent (the UI lists' labels)
}
REX_EXTERN(__imp__sub_82B49A70);
// A UI list's input step (see "the UI system's list widget"): it runs while the list has the focus.
REX_HOOK_RAW(sub_82B49A70) {
  using namespace wwe2k14::mouse_menu;
  if (REXCVAR_GET(wwe2k14_mouse_menus)) OnListStep(ctx.r3.u32);
  __imp__sub_82B49A70(ctx, base);
}
REX_EXTERN(__imp__sub_82856B40);
REX_EXTERN(__imp__sub_8282F550);
// The superstar grid's per-frame copy (see "the superstar select grid"): it runs while the grid has the input.
REX_HOOK_RAW(sub_82856B40) {
  using namespace wwe2k14::mouse_menu;
  const uint32_t w = ctx.r3.u32;
  __imp__sub_82856B40(ctx, base);
  if (REXCVAR_GET(wwe2k14_mouse_menus) && ctx.r3.u32 == 1 && w) {  // 1 = shown (it copied the cursor)
    const PPCContext after = ctx;
    OnGridFrame(ctx, base, w);
    ctx = after;
  }
}
// The select screen's move: our jump sets the walk's start to the target (the walk tests its start first).
REX_HOOK_RAW(sub_8282F550) {
  using namespace wwe2k14::mouse_menu;
  Grid& g = G();
  if (g.jump_pending && ctx.r3.u32 == g.jump_screen && ctx.r5.u32 && ctx.r6.u32) {
    g.jump_pending = false;
    rex::memory::store_and_swap<uint32_t>(Guest(ctx.r5.u32), uint32_t(g.jump_col));
    rex::memory::store_and_swap<uint32_t>(Guest(ctx.r6.u32), uint32_t(g.jump_row));
  }
  __imp__sub_8282F550(ctx, base);
}
REX_EXTERN(__imp__sub_82856E58);
// The player-slot choice's handler (the 1P / COM controller placement): once per slot a frame while the slots have the
// input.
REX_HOOK_RAW(sub_82856E58) {
  using namespace wwe2k14::mouse_menu;
  __imp__sub_82856E58(ctx, base);
  if (REXCVAR_GET(wwe2k14_mouse_menus)) OnSlotHandler();
}
REX_EXTERN(__imp__sub_8286BD68);
// The SELECT ARENA grid's vtable +148 (see "the SELECT ARENA tiles"): every frame while the grid has the input.
REX_HOOK_RAW(sub_8286BD68) {
  using namespace wwe2k14::mouse_menu;
  const uint32_t w = ctx.r3.u32;
  __imp__sub_8286BD68(ctx, base);
  if (REXCVAR_GET(wwe2k14_mouse_menus) && w && Read32(w) == kArenaVtable) OnArenaFrame(w, kArenaGrid);
}
REX_EXTERN(__imp__sub_8286D958);
// The WRESTLEMANIA ARENAS grid's vtable +148 (its per-frame copy): every frame while that grid has the input.
REX_HOOK_RAW(sub_8286D958) {
  using namespace wwe2k14::mouse_menu;
  const uint32_t w = ctx.r3.u32;
  __imp__sub_8286D958(ctx, base);
  if (REXCVAR_GET(wwe2k14_mouse_menus) && w && Read32(w) == kWmArenaGrid.vtable) OnArenaFrame(w, kWmArenaGrid);
}
REX_EXTERN(__imp__sub_8286CC68);
// The CUSTOM ARENAS grid's vtable +148 (its per-frame copy): every frame while that grid has the input.
REX_HOOK_RAW(sub_8286CC68) {
  using namespace wwe2k14::mouse_menu;
  const uint32_t w = ctx.r3.u32;
  __imp__sub_8286CC68(ctx, base);
  if (REXCVAR_GET(wwe2k14_mouse_menus) && w && Read32(w) == kCustomArenaGrid.vtable)
    OnArenaFrame(w, kCustomArenaGrid);
}
REX_EXTERN(__imp__sub_823BD358);
// The shared grid widgets' per-frame update (vtable slot 1, every grid class): the arena screen's header, on any tab.
REX_HOOK_RAW(sub_823BD358) {
  using namespace wwe2k14::mouse_menu;
  const uint32_t w = ctx.r3.u32;
  if (REXCVAR_GET(wwe2k14_mouse_menus) && w && SafeRead32(w) == kArenaVtable) OnArenaHeader(w);
  __imp__sub_823BD358(ctx, base);
}
REX_EXTERN(__imp__sub_823BC968);
// The shared grid widgets' move (every grid class; vtable +16, called every frame: it looks at the frame's presses,
// +56, itself): the arena jump's start is set here, in the call that carries our press, so no frame draws it (run
// 4364: set in the next call, a frame without the press drew the left neighbour).
// The arena's own column remap after a move (vtable +96, sub_8286BD88: rows of different lengths, LEFT from column 0
// wraps to the row's end, ...) looks up the OLD position in the screen's copy (+13540 / +13544): the copy is set to
// the jump's start as well, and after the move cursor and copy are pinned to the target (run 4365, Jarrod: edge tiles,
// e.g. Over the Limit, flashed the row's last tile).
REX_HOOK_RAW(sub_823BC968) {
  using namespace wwe2k14::mouse_menu;
  Arena& a = AR();
  const uint32_t w = ctx.r3.u32;
  bool jumped = false;
  if (a.jump_pending && w == a.jump_w && Read32(w + 56)) {
    a.jump_pending = false;
    if (wwe2k14::g_guest_frame.load(std::memory_order_relaxed) <= a.jump_until) {
      jumped = true;
      for (const uint32_t off : {kGridWRowOffset, a.jump_copy_row})
        rex::memory::store_and_swap<uint32_t>(Guest(w + off), uint32_t(a.jump_row));
      for (const uint32_t off : {kGridWColOffset, a.jump_copy_col})
        rex::memory::store_and_swap<uint32_t>(Guest(w + off), uint32_t(a.jump_col));
    }
  }
  __imp__sub_823BC968(ctx, base);
  if (jumped) {
    for (const uint32_t off : {kGridWRowOffset, a.jump_copy_row})
      rex::memory::store_and_swap<uint32_t>(Guest(w + off), uint32_t(a.jump_target_row));
    for (const uint32_t off : {kGridWColOffset, a.jump_copy_col})
      rex::memory::store_and_swap<uint32_t>(Guest(w + off), uint32_t(a.jump_target_col));
  }
}
