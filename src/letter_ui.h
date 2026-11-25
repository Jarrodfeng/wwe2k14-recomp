// S110 LETTERUI (docs\research\s110\LETTERUI.md, LETTERUI2): the letter strip of the alphabetical jump (src\letter_jump.h), DRAWN BY THE GAME ITSELF inside the exhibition
// SELECT SUPERSTAR screen: that screen is a YXMLBIN layout whose elements the game draws one by one through sub_823C54C8(ctx, element, group). The strip is
// draw-time COPIES (192 guest bytes each) of the layout's own elements: the box art (sprites), a flat red rectangle (the underline) and a text element (our UTF-8 string
// at +164: the font draws the button glyphs as characters and understands the inline colour tag). One hook on sub_823C54C8 draws the copies right after the game
// draws the panel's anchor element (so the group's transform and the resolution scaling apply); one post-hook on the panel's per-frame tick sub_82860CF0 decides the
// box, the letters and the strings. Nothing is added to the layout, no file is patched, our overlay is not used.
//
// HOW A PANEL REACHES ITS NAME BAR (LETTERUI2, listings sub_82861D50 / sub_82861BE0 / sub_828595C0 / sub_8282EB20, the layout file p13/7000):
//   The layout has 82 element groups (16-byte entries at [X + 12] = {count, first element, ..}); group 33 + k is the record "Draw_charasen0k" of panel slot k
//   (the game's table 0x82038480 / 0x820384B0: {record 119 + k, group 33 + k}, 8 bytes per slot, identical twins). Its 31 elements are the CharSet_xx GROUP
//   elements (kind 6, hidden at first). The panel keeps, in its word +37020, the index of the one that is its NAME BAR (sub_828595C0, from the row's side byte:
//   0 left -> 6 CharSet_02L, 2 right -> 7 CharSet_02R, 1 centre -> 8 CharSet_02C, list mode centre -> 9 CharSet_02CC) and sub_82861BE0 un-hides it. That group
//   element's item (+140) is the record node (children array at +100, count at +76) holding the bar's own elements: panel_waku01_L / _C / _R, font000_panel, _setting,
//   font002_name, font001_ovr, font000_ovrNum ... (file elements 39.. / 72.. / 105.. / 138..: one set per SIDE, shared by every panel of that side). The group
//   element is drawn with the panel's own transform (it sits on the panel's dummy), and its children are drawn inside it. So: panel k -> bar element
//   = group(33 + k)[panel +37020] -> node -> children; the anchor / sources are looked up in THAT child range (then, for art that lives in other nodes, in the whole
//   layout by name). Because the children are shared by the panels of one side, the draw hook notes WHICH panel's group element is being drawn (a thread-local)
//   and acts for that panel only.
//
// Every layout number lives in ONE table (letter_ui_core.h BoxLayout), overridable for a run: --wwe2k14_letter_ui_layout="k=v,k=v".
// --wwe2k14_letter_ui (default OFF until the owner has seen it): with it off the two hooks only call the game's function.
// --wwe2k14_letter_ui_dump: per panel, at each screen entry / tab change / when the cvar is turned on: its layout entry, side, bar group element, the bar node's children
// and the sources the table resolved to.
// The top row's page text comes from ONE optional provider (g_page_info), null today.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "guest_call.h"
#include "letter_jump.h"
#include "letter_ui_core.h"
#include "seats.h"  // Plausible / Readable / P / Alloc / PutBe32

REXCVAR_DECLARE(bool, wwe2k14_letter_ui);
REXCVAR_DECLARE(bool, wwe2k14_letter_ui_dump);
REXCVAR_DECLARE(std::string, wwe2k14_letter_ui_layout);

REX_EXTERN(__imp__sub_82860CF0);
REX_EXTERN(__imp__sub_823C54C8);

namespace wwe2k14::letter_ui {

constexpr uint32_t kLayoutMgrPtr = 0x839294B4;  // L; the select screen's layout is [L + 12] (LETTERUI.md claim 2)
constexpr uint32_t kElemSize = 192;             // an element = 192 bytes at [X + 8] + 192 i, count [X + 24]; groups: 16 bytes at [X + 12] + 16 g, count [X + 28]
constexpr uint32_t kPanelTable = 0x82036A10;    // the panel-layout table: 156 bytes per layout (claim 6)
constexpr uint32_t kPanelEntries = 17;
constexpr uint32_t kEntrySize = 156;
constexpr uint32_t kScreenEntry = 48432;        // [screen + 48432] = the chosen layout entry
constexpr uint32_t kScreenList = 48428;         // [screen + 48428] == 1 = the list mode
constexpr uint32_t kScreenPanels = 44;          // [screen + 44 + 4 k] = panel k
constexpr uint32_t kPanelIndex = 276;           // [panel + 276] = the panel's slot k
constexpr uint32_t kPanelBarSlot = 37020;       // [panel + 37020] = the index of its name bar group element inside group 33 + k (sub_828595C0)
constexpr uint32_t kPanelAttireSlot = 37036;    // [panel + 37036] = the index of its CharSet_03x (attire box) group element (dump only)
constexpr uint32_t kBarTable = 0x82038480;      // {record 119 + k, group 33 + k} pairs, 8 bytes per slot from HERE (first word = record, the group is the SECOND word); twin 0x820384B0
constexpr uint32_t kMaxChildren = 200;
constexpr int kSlots = 6;
constexpr int kParts = 8;                       // L, C, R, underline, top text, bottom text, left pill, right pill
enum Part { kL = 0, kC = 1, kR = 2, kUl = 3, kT1 = 4, kT2 = 5, kPl = 6, kPr = 7 };

// THE ONE COUPLING with the downloaded-tab pager (src\seat_bind.h): page k of n for a panel; false / pages <= 0 = no page text. Null today.
inline std::function<bool(uint32_t panel, int& page, int& pages)> g_page_info;

struct Slot {
  uint32_t panel = 0, anchor = 0, bar = 0, pool = 0;
  uint32_t src[kParts] = {}, copy[kParts] = {}, str[2] = {};
  bool active = false, built = false, dumped = false, log_world = false;  // built: the strip for `key` was decided (active: and it is drawable)
  uint64_t key = 0;
  uint32_t dump_tab = 99;
  Box box = Box::None;
  int side = 1;
  bool top = false, has_row2 = false, pills = false;
  float cur_dx = 0;  // the current letter's x from the text's centre (UnderlineOffset)
};
inline Slot g_slot[kSlots];
inline std::atomic<uint32_t> g_anchor[kSlots];  // published for the draw hook: the anchor child
inline std::atomic<uint32_t> g_bar[kSlots];     // published for the draw hook: the panel's bar GROUP element
inline BoxLayout g_layout;
inline std::string g_layout_raw = "\x01";       // forces the first parse
inline uint32_t g_layout_gen = 0;
inline std::atomic<bool> g_logged_first{false};
inline thread_local int g_draw_depth = 0;
inline thread_local int g_cur_panel = -1;       // the slot whose bar group element the game is drawing right now

inline float RdF(uint8_t* base, uint32_t a) {
  const uint32_t v = seats::U32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void WrF(uint8_t* h, uint32_t off, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  seats::PutBe32(h + off, v);
}

inline const BoxLayout& CurLayout() {
  const std::string raw = REXCVAR_GET(wwe2k14_letter_ui_layout);
  if (raw != g_layout_raw) {
    g_layout_raw = raw;
    g_layout = BoxLayout();
    std::string err;
    const int n = ParseLayout(raw, g_layout, &err);
    ++g_layout_gen;
    if (!raw.empty()) REXLOG_INFO("[wwe2k14][letter_ui] layout override '{}': {} applied{}{}", raw, n, err.empty() ? "" : "; problems: ", err);
  }
  return g_layout;
}

// ---------------------------------------------------------------------------------------------------------------- layout access
struct Elems {
  uint32_t arr = 0, count = 0;      // the flat element array
  uint32_t outer = 0, outer_n = 0;  // the 16-byte group entries {count, first element}
  uint32_t At(uint32_t i) const { return i < count ? arr + kElemSize * i : 0; }
};

inline bool ElemsOf(uint8_t* base, Elems& e) {
  const uint32_t L = seats::U32(base, kLayoutMgrPtr);
  if (!seats::Plausible(L)) return false;
  const uint32_t X = seats::U32(base, L + 12);
  if (!seats::Plausible(X)) return false;
  e.arr = seats::U32(base, X + 8);
  e.count = seats::U32(base, X + 24);
  e.outer = seats::U32(base, X + 12);
  e.outer_n = seats::U32(base, X + 28);
  return seats::Plausible(e.arr) && e.count > 0 && e.count < 4096 && seats::Readable(e.arr, kElemSize * e.count) && seats::Plausible(e.outer) && e.outer_n > 0 &&
         e.outer_n < 512 && seats::Readable(e.outer, 16 * e.outer_n);
}

// An element's name: the two 16-byte halves at +72 / +88 joined (the game's own lookup, sub_823C3078, matches "%s%s" of them).
inline std::string NameOfElem(uint8_t* base, uint32_t el) {
  std::string s;
  for (uint32_t half : {72u, 88u}) {
    const uint8_t* p = seats::P(base, el + half);
    for (uint32_t i = 0; i < 16 && p[i]; ++i) s.push_back(char(p[i]));
  }
  return s;
}

// The panel's name bar (see the header comment): the group element, its node, the node's children range.
struct BarRef {
  uint32_t group = 0, slot = 0;           // outer group and the element index inside it
  uint32_t elem = 0, node = 0;            // the bar group element and its item (the record node)
  uint32_t first = 0, count = 0;          // the node's children: guest address of the first and how many (192 bytes each)
  bool ok = false;
};
inline BarRef BarOf(uint8_t* base, const Elems& e, uint32_t panel, uint32_t slot_word) {
  BarRef b;
  const uint32_t k = seats::U32(base, panel + kPanelIndex);
  if (k > 5) return b;
  uint32_t words[12];
  for (uint32_t i = 0; i < 12; ++i) words[i] = seats::U32(base, kBarTable + 4 * i);
  b.group = GroupFromTable(words, int(k));
  b.slot = slot_word;
  if (b.group >= e.outer_n) return b;
  const uint32_t entry = e.outer + 16 * b.group, cnt = seats::U32(base, entry), ptr = seats::U32(base, entry + 4);
  if (b.slot >= cnt || !seats::Plausible(ptr) || !seats::Readable(ptr, kElemSize * cnt)) return b;
  b.elem = ptr + kElemSize * b.slot;
  b.node = seats::U32(base, b.elem + 140);
  if (!seats::Plausible(b.node) || !seats::Readable(b.node, 124)) return b;
  b.first = seats::U32(base, b.node + 100);
  b.count = seats::U32(base, b.node + 76);
  b.ok = b.count > 0 && b.count <= kMaxChildren && seats::Plausible(b.first) && seats::Readable(b.first, kElemSize * b.count);
  return b;
}

inline uint32_t FindIn(uint8_t* base, uint32_t first, uint32_t count, const std::string& name) {
  for (uint32_t i = 0; i < count; ++i)
    if (NameOfElem(base, first + kElemSize * i) == name) return first + kElemSize * i;
  return 0;
}

inline uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }

inline void Deactivate(int k) {
  Slot& s = g_slot[k];
  s.active = false, s.built = false, s.dumped = false, s.log_world = false, s.key = 0, s.anchor = 0, s.bar = 0;
  g_anchor[k].store(0, std::memory_order_relaxed);
  g_bar[k].store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------- the dump
inline void DumpElem(uint8_t* base, const char* tag, uint32_t idx, uint32_t el) {
  const uint32_t item = seats::U32(base, el + 140);
  const uint32_t kind = seats::Readable(item, 8) ? seats::U32(base, item + 4) : 0xFFFFFFFFu;
  REXLOG_INFO("[wwe2k14][letter_ui]   {} #{} '{}' kind {} hidden {} grp {} pos ({:.1f},{:.1f}) scale ({:.3f},{:.3f}) size ({:.1f},{:.1f}) rot {:.1f} rt ({:.1f},{:.1f}) rtscale ({:.3f},{:.3f}) anchorbyte {}", tag,
              idx, NameOfElem(base, el), int(kind), seats::U32(base, el + 144), int(*seats::P(base, el + 113)), RdF(base, el + 4), RdF(base, el + 8), RdF(base, el + 12), RdF(base, el + 16),
              RdF(base, el + 20), RdF(base, el + 24), RdF(base, el + 28), RdF(base, el + 120), RdF(base, el + 124), RdF(base, el + 132), RdF(base, el + 136), int(*seats::P(base, el + 52)));
}
inline void DumpPanel(uint8_t* base, const Slot& s, int k, uint32_t entry, bool has_vs, uint32_t tab, const Elems& e, const BarRef& bar, uint32_t slot_word, bool list_mode,
                      const uint32_t src[kParts], uint32_t anchor, const std::string& box_base) {
  const uint32_t attire = seats::U32(base, s.panel + kPanelAttireSlot);
  const int expect = BarSlotFor(s.side, list_mode);
  REXLOG_INFO("[wwe2k14][letter_ui] panel k={} {:08X} slot {} entry {} side {} kind {} vs {} size {} tab {} | bar: group {} (table word {}) slot word {} (expected {}{}) element {:08X} #{} '{}' item {:08X} children "
              "[{:08X} x{}] = #{}..#{} | attire slot word {} | layout gen {}",
              k, s.panel, seats::U32(base, s.panel + kPanelIndex), (entry - kPanelTable) / kEntrySize, s.side, int(seats::P(base, entry + 12 + 24 * uint32_t(k))[0]), has_vs ? 1 : 0,
              s.box == Box::Regular ? "regular" : s.box == Box::Small ? "small" : "none", tab, bar.group, seats::U32(base, kBarTable + 8 * seats::U32(base, s.panel + kPanelIndex) + 4), slot_word,
              expect, std::string(slot_word == uint32_t(expect) ? " = as the side says" : " MISMATCH") + (bar.elem && NameOfElem(base, bar.elem).rfind("CharSet_", 0) == 0 ? "" : " NAME-MISMATCH (not CharSet_)") +
                  " [table entry words " + std::to_string(seats::U32(base, kBarTable + 8 * seats::U32(base, s.panel + kPanelIndex))) + ", " +
                  std::to_string(seats::U32(base, kBarTable + 8 * seats::U32(base, s.panel + kPanelIndex) + 4)) + "]",
              bar.elem, bar.elem ? (bar.elem - seats::U32(base, e.outer + 16 * bar.group + 4)) / kElemSize : 0u,
              bar.elem ? NameOfElem(base, bar.elem) : "?", bar.node, bar.first, bar.count, bar.first ? (bar.first - e.arr) / kElemSize : 0u,
              bar.first ? (bar.first - e.arr) / kElemSize + bar.count - 1 : 0u, attire, g_layout_gen);
  if (bar.ok)
    for (uint32_t i = 0; i < bar.count; ++i) DumpElem(base, "bar child", (bar.first - e.arr) / kElemSize + i, bar.first + kElemSize * i);
  static const char* names[kParts] = {"box L", "box C", "box R", "underline", "text (top)", "text (bottom)", "pill L", "pill R"};
  REXLOG_INFO("[wwe2k14][letter_ui]   resolved: anchor {:08X} {} | box art '{}'", anchor, anchor ? "#" + std::to_string((anchor - e.arr) / kElemSize) + " " + NameOfElem(base, anchor) : "-", box_base);
  for (int i = 0; i < kParts; ++i)
    if (src[i] && i != kT1) DumpElem(base, names[i], (src[i] - e.arr) / kElemSize, src[i]);
}

// ---------------------------------------------------------------------------------------------------------------- the panel tick
inline void OnTick(PPCContext& ctx, uint8_t* base, uint32_t panel) {
  if (!REXCVAR_GET(wwe2k14_letter_ui)) {
    for (int k = 0; k < kSlots; ++k)
      if (g_slot[k].active || g_slot[k].built) Deactivate(k);
    return;
  }
  auto deactivate_panel = [&](uint32_t p) {
    for (int k = 0; k < kSlots; ++k)
      if ((g_slot[k].active || g_slot[k].built) && g_slot[k].panel == p) Deactivate(k);
  };
  scene_director::Caller call(ctx, base);
  letter_jump::Gated g;
  if (!letter_jump::LetterJumpOn() || !letter_jump::GateOf(call, base, panel, g)) return deactivate_panel(panel);

  int k = -1;
  for (int i = 0; i < kSlots; ++i)
    if (seats::U32(base, g.screen + kScreenPanels + 4 * uint32_t(i)) == panel) k = i;
  const uint32_t entry = seats::U32(base, g.screen + kScreenEntry);
  if (k < 0 || entry < kPanelTable || entry >= kPanelTable + kPanelEntries * kEntrySize || (entry - kPanelTable) % kEntrySize != 0 || !seats::Readable(entry, kEntrySize))
    return deactivate_panel(panel);
  PanelRow rows[6];
  for (int i = 0; i < 6; ++i) {
    const uint8_t* r = seats::P(base, entry + 12 + 24 * uint32_t(i));
    rows[i].kind = r[0];
    rows[i].side = r[2];
  }
  const bool has_vs = seats::U32(base, entry + 8) != 0xFFFFFFFFu;
  const Box box = SizeFor(rows, k, has_vs);
  if (box == Box::None) return deactivate_panel(panel);

  const BoxLayout& L = CurLayout();
  Slot& s = g_slot[k];
  const uint32_t cur = call(sub_828568B8, {panel});
  const bool dump = REXCVAR_GET(wwe2k14_letter_ui_dump);
  int page = 0, pages = 0;
  if (g.tab == 3 && g_page_info && box == Box::Regular && !g_page_info(panel, page, pages)) page = pages = 0;
  Elems e;
  if (!ElemsOf(base, e)) return deactivate_panel(panel);
  const uint32_t list = seats::U32(base, panel + letter_jump::kListShown);
  const uint32_t slot_word = seats::U32(base, panel + kPanelBarSlot);
  uint64_t key = 1469598103934665603ull;
  for (uint64_t v : {uint64_t(panel), uint64_t(cur), uint64_t(g.tab), uint64_t(list), uint64_t(e.arr), uint64_t(entry), uint64_t(box), uint64_t(page), uint64_t(pages),
                     uint64_t(g_layout_gen), uint64_t(dump), uint64_t(slot_word),
                     uint64_t(seats::U32(base, panel + letter_jump::kListCount + letter_jump::kListStride * (list > 5 ? 0 : list)))})
    key = Mix(key, v);
  if (s.built && s.panel == panel && s.key == key) return;

  // ---- rebuild: the panel's name bar, then the sources by name (in the bar's node first, then the whole layout)
  const BarRef bar = BarOf(base, e, panel, slot_word);
  if (!bar.ok) {
    if (dump) REXLOG_INFO("[wwe2k14][letter_ui] panel k={}: no name bar found (group {} slot word {} outer groups {})", k, bar.group, slot_word, e.outer_n);
    s.key = key, s.built = true, s.active = false;
    g_anchor[k].store(0, std::memory_order_relaxed);
    g_bar[k].store(0, std::memory_order_relaxed);
    return;
  }
  auto find = [&](const std::string& name) {
    const uint32_t a = FindIn(base, bar.first, bar.count, name);
    return a ? a : FindIn(base, e.arr, e.count, name);
  };
  const uint32_t anchor = FindIn(base, bar.first, bar.count, L.anchor);
  uint32_t src[kParts] = {};
  const std::vector<std::string> bases = SplitList(L.box_src);
  const int bi = FirstResolving(bases, [&](const std::string& b) { return find(b + "_L") && find(b + "_C") && find(b + "_R"); });
  const std::string box_base = bi >= 0 ? bases[size_t(bi)] : std::string("(none resolved)");
  if (bi >= 0) {
    src[kL] = find(bases[size_t(bi)] + "_L");
    src[kC] = find(bases[size_t(bi)] + "_C");
    src[kR] = find(bases[size_t(bi)] + "_R");
  }
  src[kUl] = find(L.src_ul);
  src[kT1] = src[kT2] = find(L.src_text);
  const bool pills = L.use_pills != 0.0f;
  if (pills) src[kPl] = find(L.pill_l), src[kPr] = find(L.pill_r);
  s.panel = panel, s.side = rows[k].side, s.box = box;
  if (dump && (!s.dumped || s.dump_tab != g.tab)) {
    DumpPanel(base, s, k, entry, has_vs, g.tab, e, bar, slot_word, seats::U32(base, g.screen + kScreenList) == 1, src, anchor, box_base);
    s.dumped = true, s.dump_tab = g.tab, s.log_world = true;
  } else if (!dump) {
    s.dumped = false;
  }
  bool all = anchor != 0;
  for (int i = 0; i < kParts; ++i)
    if ((i == kPl || i == kPr) ? pills : true) all = all && src[i] != 0;
  if (!all) {
    REXLOG_INFO("[wwe2k14][letter_ui] panel k={}: a source element was not found (anchor {:08X} box '{}' L {:08X} C {:08X} R {:08X} underline {:08X} text {:08X} pills {:08X}/{:08X}): no strip (see --wwe2k14_letter_ui_dump)",
                k, anchor, box_base, src[kL], src[kC], src[kR], src[kUl], src[kT2], src[kPl], src[kPr]);
    s.key = key, s.built = true;  // decided for this key: do not retry every frame
    s.active = false;
    g_anchor[k].store(0, std::memory_order_relaxed);
    g_bar[k].store(0, std::memory_order_relaxed);
    return;
  }
  letter_jump::PanelLetters pl;
  if (!letter_jump::LettersOf(call, base, panel, pl)) pl = letter_jump::PanelLetters();
  const std::string row2 = StripRow2(pl.present, pl.cur, box, L.dim, !pills);
  const std::string row1 = (box == Box::Regular) ? StripRow1(g.tab == 3, page, pages) : std::string();
  s.cur_dx = UnderlineOffset(RunFor(pl.present, pl.cur, box, L, box == Box::Regular ? L.text_scale : L.small_scale, !pills));
  if (!s.pool) s.pool = seats::Alloc(nullptr, kParts * kElemSize + 2 * 512);
  if (!s.pool) return;
  for (int i = 0; i < kParts; ++i) s.copy[i] = s.pool + kElemSize * uint32_t(i);
  s.str[0] = s.pool + kParts * kElemSize, s.str[1] = s.str[0] + 512;
  const std::string* strs[2] = {&row1, &row2};
  for (int i = 0; i < 2; ++i) {
    const size_t n = std::min<size_t>(strs[i]->size(), 511);
    uint8_t* h = seats::P(base, s.str[i]);
    std::memcpy(h, strs[i]->data(), n);
    h[n] = 0;
  }
  std::memcpy(s.src, src, sizeof src);
  s.anchor = anchor, s.bar = bar.elem, s.pills = pills, s.top = !row1.empty(), s.has_row2 = !row2.empty(), s.key = key, s.built = true, s.active = s.has_row2;
  g_anchor[k].store(s.has_row2 ? anchor : 0, std::memory_order_relaxed);
  g_bar[k].store(s.has_row2 ? bar.elem : 0, std::memory_order_relaxed);
  if (!g_logged_first.exchange(true))
    REXLOG_INFO("[wwe2k14][letter_ui] first strip: panel {:08X} k={} box {} tab {} letters {} current '{}' bar group {} slot {} (box art '{}')", panel, k, box == Box::Regular ? "regular" : "small", g.tab,
                pl.present.size(), pl.cur ? pl.cur : '?', bar.group, bar.slot, box_base);
}

// ---------------------------------------------------------------------------------------------------------------- the draw
// Called right after the game drew `anchor` (r3 = ctx, r4 = element, r5 = group of that call) inside panel k's bar group element.
inline void OnDraw(PPCContext& ctx, uint8_t* base, uint32_t r3, uint32_t r4, uint32_t r5, int k) {
  if (k < 0 || k >= kSlots || g_anchor[k].load(std::memory_order_relaxed) != r4 || g_draw_depth > 0) return;
  Slot& s = g_slot[k];
  if (!s.active || !seats::Readable(r4, kElemSize)) return;
  const uint8_t grp = *seats::P(base, r4 + 113);
  if (seats::U32(base, r4 + 144) != 0 || (grp != 0 && uint32_t(int8_t(grp)) != r5)) return;  // the game did not draw it (sub_823C54C8 +14 / +28)
  const BoxLayout& L = CurLayout();
  for (int i = 0; i < kParts; ++i)
    if (s.src[i] && !seats::Readable(s.src[i], kElemSize)) return;
  g_draw_depth++;
  // refresh every copy from its source FIRST (the game rewrites its elements every frame), then write our fields
  for (int i = 0; i < kParts; ++i)
    if (s.src[i]) std::memcpy(seats::P(base, s.copy[i]), seats::P(base, s.src[i]), kElemSize);
  auto F = [&](int part, uint32_t off) { return RdF(base, s.copy[part] + off); };
  auto host = [&](int part) { return seats::P(base, s.copy[part]); };
  // the copies are drawn inside the anchor's group: no runtime offset / scale of their own, drawn, no multiplier but ours
  auto common = [&](int part) {
    uint8_t* h = host(part);
    h[113] = grp;
    seats::PutBe32(h + 144, 0);
    WrF(h, 120, 0), WrF(h, 124, 0), WrF(h, 128, 0);
    WrF(h, 132, 1), WrF(h, 136, 1);
  };
  auto mult = [&](int part, float r, float gg, float b, float a) {
    uint8_t* h = host(part);
    WrF(h, 148, r), WrF(h, 152, gg), WrF(h, 156, b), WrF(h, 160, a);
  };
  const Placed p = PlaceBox(L, s.box, s.top, s.side, s.cur_dx);
  PieceSizes n;  // the drawn native sizes: size field x scale
  n.lw = F(kL, 20) * F(kL, 12), n.cw = F(kC, 20) * F(kC, 12), n.rw = F(kR, 20) * F(kR, 12), n.h = F(kC, 24) * F(kC, 16);
  const BarPlace b = PlaceBar(p, n, L, s.side);
  if (b.refused) {  // an impossible box (a layout override): draw nothing rather than overlapping caps
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_WARN("[wwe2k14][letter_ui] letter strip: the box is too narrow for its end pieces (box {:.1f} x {:.1f}, caps {:.1f} + {:.1f}, mid_min {:.1f})", p.w, p.h, b.left_cap, b.right_cap, L.mid_min);
    g_draw_depth--;
    return;
  }
  {  // the box: left cap position, middle, right cap position (the middle first: the caps cover its ends)
    const int lp = b.swap ? int(kR) : int(kL), rp = b.swap ? int(kL) : int(kR);
    struct One { int part; float x, sx; };
    const One ones[3] = {{kC, b.xc, b.sxc}, {lp, b.xl, b.sxl}, {rp, b.xr, b.sxr}};
    for (const One& o : ones) {
      common(o.part);
      uint8_t* h = host(o.part);
      WrF(h, 4, o.x);
      WrF(h, 8, p.cy);
      WrF(h, 12, F(o.part, 12) * o.sx);
      WrF(h, 16, F(o.part, 16) * b.sy);
      mult(o.part, L.box_r, L.box_g, L.box_b, L.box_a);
    }
  }
  {  // the underline: the flat rectangle, centred, scaled to ul_w x ul_h, its own colour REPLACED (the rectangle's base colour fields +36..+48)
    common(kUl);
    uint8_t* h = host(kUl);
    WrF(h, 4, p.ul_x);
    WrF(h, 8, p.ul_y);
    WrF(h, 12, F(kUl, 20) > 0.5f ? L.ul_w / F(kUl, 20) : 1.0f);
    WrF(h, 16, F(kUl, 24) > 0.5f ? L.ul_h / F(kUl, 24) : 1.0f);
    WrF(h, 36, L.ul_r), WrF(h, 40, L.ul_g), WrF(h, 44, L.ul_b), WrF(h, 48, L.ul_a);
    mult(kUl, 1, 1, 1, 1);
  }
  const float sy_row[2] = {p.row1_y, p.row2_y}, sc_row[2] = {p.row_scale1, p.row_scale2};
  for (int i = 0; i < 2; ++i) {  // text: centred in the box, absolute scale
    const int part = kT1 + i;
    common(part);
    uint8_t* h = host(part);
    WrF(h, 4, p.text_x);
    WrF(h, 8, sy_row[i]);
    WrF(h, 12, sc_row[i]);
    WrF(h, 16, sc_row[i]);
    mult(part, 1, 1, 1, 1);
    seats::PutBe32(h + 164, s.str[i]);
  }
  if (s.pills) {
    const float px[2] = {p.pill_lx, p.pill_rx};
    for (int i = 0; i < 2; ++i) {
      const int part = kPl + i;
      common(part);
      uint8_t* h = host(part);
      WrF(h, 4, px[i]);
      WrF(h, 8, p.row2_y);
      WrF(h, 12, L.pill_scale);
      WrF(h, 16, L.pill_scale);
      mult(part, 1, 1, 1, 1);
    }
  }
  if (s.log_world) {  // the anchor's world position, once per dump: ctx + 192 / 196 = the group's origin, + 208 / 212 its scale (sub_823C54C8 +1F4..+2CC)
    s.log_world = false;
    const float ox = RdF(base, r3 + 192), oy = RdF(base, r3 + 196), sx = RdF(base, r3 + 208), sy = RdF(base, r3 + 212);
    REXLOG_INFO("[wwe2k14][letter_ui] panel k={} anchor world: bar group origin ({:.1f},{:.1f}) scale ({:.3f},{:.3f}){} | native sizes L {:.1f} C {:.1f} R {:.1f} h {:.1f} | box centre ({:.1f},{:.1f}) {:.1f} x {:.1f} -> "
                "caps {:.1f} / {:.1f}, middle {:.1f} (x {:.1f} / {:.1f} / {:.1f}), underline ({:.1f},{:.1f}) current letter dx {:.1f}",
                k, ox, oy, sx, sy, sx < 0 ? " MIRRORED" : "", n.lw, n.cw, n.rw, n.h, p.cx, p.cy, p.w, p.h, b.left_cap, b.right_cap, b.mid_w, b.xl, b.xc, b.xr, p.ul_x, p.ul_y, s.cur_dx);
  }
  scene_director::Caller call(ctx, base);
  static const int order[] = {kC, kL, kR, kUl, kT1, kT2, kPl, kPr};
  for (int part : order) {
    if (part == kT1 && !s.top) continue;
    if ((part == kPl || part == kPr) && !s.pills) continue;
    call(__imp__sub_823C54C8, {r3, s.copy[part], r5});
  }
  g_draw_depth--;
}

}  // namespace wwe2k14::letter_ui

// ---------------------------------------------------------------------------------------------------------------- the hooks
REX_HOOK_RAW(sub_82860CF0) {  // the panel's per-frame tick (r3 = the panel; LETTERUI.md claim 5)
  const uint32_t panel = ctx.r3.u32;
  __imp__sub_82860CF0(ctx, base);
  wwe2k14::letter_ui::OnTick(ctx, base, panel);
}

REX_HOOK_RAW(sub_823C54C8) {  // the element draw (r3 = ctx, r4 = element, r5 = group); the strip is drawn right after the panel's anchor element
  namespace lu = wwe2k14::letter_ui;
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  int entered = -1, prev = -1;
  for (int i = 0; i < lu::kSlots; ++i)  // a panel's bar GROUP element: its children (shared by the panels of one side) are drawn for that panel
    if (lu::g_bar[i].load(std::memory_order_relaxed) == r4 && r4 != 0) entered = i;
  if (entered >= 0) prev = lu::g_cur_panel, lu::g_cur_panel = entered;
  __imp__sub_823C54C8(ctx, base);
  if (entered >= 0) {
    lu::g_cur_panel = prev;
    return;
  }
  if (lu::g_cur_panel >= 0) lu::OnDraw(ctx, base, r3, r4, r5, lu::g_cur_panel);
}
