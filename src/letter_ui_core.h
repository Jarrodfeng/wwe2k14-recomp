// S110 LETTERUI (docs\research\s110\LETTERUI.md, LETTERUI2): the pure half of the letter strip drawn by the game's own menu system on the exhibition
// SELECT SUPERSTAR screen. std only, no game memory. The game side is src\letter_ui.h; the unit is tools\letter_ui\letter_ui_unit.cpp.
//
//   * SizeFor        which box a panel gets (the owner's rules, per SIDE): Regular (beside the render), Small (beside the name bar), None.
//   * StripRow1 / 2  the two strings the game's font draws: button glyphs are characters of the font (U+E00B LB, U+E00C LT, U+E00E RB, U+E00F RT) and an
//                    inline colour tag `<$COLOR #RRGGBBAA>` .. `</COLOR>` dims the neighbours (the current letter is untagged).
//   * BarSlotFor / BarElemOf   HOW A PANEL REACHES ITS NAME BAR (LETTERUI2): the panel's bar is ONE element of the layout's group 33 + k (the game's own
//                    "Draw_charasen0k" record, one per panel slot k) at the index the panel keeps in its word +37020; that element is a group whose node holds
//                    the bar's pieces. Nothing is searched by "first element + N" any more.
//   * BoxLayout      EVERY layout number in one table, each with where it came from; the same numbers can be overridden for one run by
//                    --wwe2k14_letter_ui_layout="k=v,k=v" (ParseLayout). Numbers are in the layout's 640 x 360 space, in the bar group's space: origin = the
//                    bar's centre (the group sits on the panel's dummy_1P), x grows to the right, y downward.
//   * PlaceBox       where the box and its text go for a side (mirrored about the bar's centre = about x = 320 of the screen: the outer side is taken from the
//                    panel's side byte; the TEXT order never changes).
//   * PlaceBar       how the box's three pieces (CENTRE-anchored sprites, copied) are stretched to the box: by centres, caps meet the middle at any width.
//   * UnderlineOffset where the current letter's centre lies in the centred string (from glyph advances).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace wwe2k14::letter_ui {

enum class Box { None, Small, Regular };

// One row of a panel-layout table entry (src\letter_ui.h reads them from 0x82036A10 + 156 e + 12 + 24 k: kind byte +0, side byte +2).
struct PanelRow {
  int kind = 14;  // 0 normal, 1 referee, 2 extra / manager, 14 none
  int side = 1;   // 0 left, 1 centre, 2 right
};

// The owner's rule (LETTERUI.md section 3): Regular iff the layout has a VS record and the panel's side holds exactly ONE kind-0 row and it is not the centre
// side; Small for every other kind-0 panel; None for a referee / manager / empty row.
inline Box SizeFor(const PanelRow rows[6], int k, bool has_vs) {
  if (k < 0 || k > 5 || rows[k].kind != 0) return Box::None;
  int same = 0;
  for (int i = 0; i < 6; ++i)
    if (rows[i].kind == 0 && rows[i].side == rows[k].side) ++same;
  return (has_vs && same == 1 && rows[k].side != 1) ? Box::Regular : Box::Small;
}

// ---------------------------------------------------------------------------------------------------------------- the panel's name bar
// [C] sub_828595C0 (called from sub_8282EB20 with the row's side byte) stores, per side, the element index of the bar inside the panel's group at panel +37020:
//   side 0 (left) 6 = CharSet_02L, side 2 (right) 7 = CharSet_02R, side 1 (centre) 8 = CharSet_02C, or 9 = CharSet_02CC when the screen is the list mode.
constexpr int kBarSlotLeft = 6, kBarSlotRight = 7, kBarSlotCentre = 8, kBarSlotCentreList = 9;
inline int BarSlotFor(int side, bool list_mode) { return side == 0 ? kBarSlotLeft : side == 2 ? kBarSlotRight : (list_mode ? kBarSlotCentreList : kBarSlotCentre); }

struct ElemRef {
  int group = -1;  // the layout's outer group (the 16-byte entries at [X + 12])
  int index = -1;  // the element inside it
  bool operator==(const ElemRef& o) const { return group == o.group && index == o.index; }
};
// `group_of_panel[k]` = the second word of the game's per-panel table (0x82038484 + 8 k: 33 + k), `slot` = the panel's +37020 word.
inline ElemRef BarElemOf(const int group_of_panel[6], int k, int slot) {
  ElemRef r;
  if (k < 0 || k > 5 || slot < 0) return r;
  r.group = group_of_panel[k];
  r.index = slot;
  return r;
}

// The game's per-panel table (0x82038480, 8 bytes per slot): word 2k = the record (119 + k), word 2k + 1 = the GROUP (33 + k). The group is the SECOND word.
inline int GroupFromTable(const uint32_t words[12], int k) { return (k < 0 || k > 5) ? -1 : int(words[2 * k + 1]); }

// "a|b|c" -> {"a", "b", "c"} (empty items dropped)
inline std::vector<std::string> SplitList(const std::string& s) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t bar = s.find('|', pos);
    if (bar == std::string::npos) bar = s.size();
    if (bar > pos) out.push_back(s.substr(pos, bar - pos));
    pos = bar + 1;
  }
  return out;
}
// The first entry of `bases` for which `resolves(base)` holds: its index, -1 = none ("first that resolves wins").
template <class F>
inline int FirstResolving(const std::vector<std::string>& bases, F&& resolves) {
  for (size_t i = 0; i < bases.size(); ++i)
    if (resolves(bases[i])) return int(i);
  return -1;
}

// ---------------------------------------------------------------------------------------------------------------- strings
// The font's button glyphs (LETTERUI.md claim 10, UTF-8 of U+E00B / E00C / E00E / E00F).
inline const char* GlyphLB() { return "\xEE\x80\x8B"; }
inline const char* GlyphLT() { return "\xEE\x80\x8C"; }
inline const char* GlyphRB() { return "\xEE\x80\x8E"; }
inline const char* GlyphRT() { return "\xEE\x80\x8F"; }

inline std::string Dimmed(char c, const std::string& rgba_hex) { return "<$COLOR #" + rgba_hex + ">" + std::string(1, c) + "</COLOR>"; }

// `present` = the letters present, ascending and distinct (letter_jump::PresentLetters); the letters beside `cur` in that order, wrapping at both ends.
// 0 = none: fewer than two letters present means no neighbour.
inline void Neighbours(const std::vector<char>& present, char cur, char& prev, char& next) {
  prev = next = 0;
  char lo = 0, hi = 0;  // the smallest / largest present letter other than cur: the wrap targets
  for (char c : present) {
    if (c == cur) continue;
    if (!lo) lo = c;
    hi = c;
    if (c < cur) prev = c;             // the largest below cur
    if (c > cur && !next) next = c;    // the smallest above cur
  }
  if (!prev) prev = hi;  // wrap
  if (!next) next = lo;
}

// The bottom row: "LT prev CURRENT next RT" (Regular), "LT CURRENT RT" (Small, one letter's width is all the room). prev / next are tagged dim; the current
// letter is plain. "" = no letter to show (cur == 0). glyphs = false leaves the LT / RT characters out (the pill option: the game's own LT / RT elements draw them).
inline std::string StripRow2(const std::vector<char>& present, char cur, Box box, const std::string& dim_rgba, bool glyphs = true) {
  if (cur == 0 || box == Box::None) return {};
  std::string s = glyphs ? std::string(GlyphLT()) + ' ' : std::string();
  char prev = 0, next = 0;
  if (box == Box::Regular) Neighbours(present, cur, prev, next);
  if (prev) s += Dimmed(prev, dim_rgba) + ' ';
  s += cur;
  if (next) s += ' ' + Dimmed(next, dim_rgba);
  if (glyphs) {
    s += ' ';
    s += GlyphRT();
  }
  return s;
}

// The top row: "LB PAGE k / n RB", the DOWNLOADED tab with a page provider only (pages <= 0 = no provider / not paged: nothing, and the box shrinks to one row).
inline std::string StripRow1(bool downloaded_tab, int page, int pages) {
  if (!downloaded_tab || pages <= 0 || page < 1) return {};
  return std::string(GlyphLB()) + " PAGE " + std::to_string(page) + " / " + std::to_string(pages) + " " + GlyphRB();
}

// ---------------------------------------------------------------------------------------------------------------- the layout table
// Marks: [file] read from the layout file's own element (MnLayHD p13/7000), [measured] from the look lane's runs of 2026-11-24 (converted in the comment),
// [guess] a number nobody has seen drawn yet (tune after the look run).
struct BoxLayout {
  // Which of the layout's own elements the copies are made from. Every name is the two name halves joined; a name is looked for in the PANEL'S BAR NODE first
  // (the children of the bar group element, see BarElemOf), then in the whole layout (first element of that name). Lists: "a|b" = the first that resolves.
  std::string anchor = "font001_ovr";                  // the bar node's child after whose draw the strip is drawn: OVERALL text [file: #45 / #78 / #111 / #144]
  std::string box_src = "panel_waku02|panel_waku01";   // the box art: the base name + _L / _C / _R (centre-anchored sprites). waku02 = the game's ATTIRE / FINISHERS box
                                                       // (32 high, white rim, slanted) [file #151-154], waku01 = the name bar itself [file #39-41]
  std::string src_text = "font002_name";               // the text copies: the name's italic face; font001_ovr squeezes the string into 48 units [measured run 6 / 7]
  std::string src_ul = "beta_C";                       // the underline: a flat 68 x 10 red rectangle (kind 2, base colour 0.8235 / 0.0196 / 0.0196 / 1) [file #1263]
  std::string pill_l = "font000_LT", pill_r = "font000_RT";  // the game's own LT / RT glyph elements [file #159 / #160]: only with use_pills
  std::string dim = "FFFFFF66";                        // RRGGBBAA of the neighbouring letters [measured run 5: alpha dims, byte order right]

  // The box, centre and size. Origin = the bar's centre; the outward sign is - for the left side, + for the right (and the centre side).
  float reg_w = 104;     // Regular box width [measured run 13: x 17.5 .. 121.5 of 640]
  // THE BOX RULE (lane STRIPTUNE, Jarrod 2026-11-24 09:08 "C is probably my favorite"): the two slanted end pieces (caps) are scaled EVENLY with the box height
  // (cap_keep_shape 1: cap width = native cap width x h / native h, i.e. 32 x h / 32 = h for the attire art), so the slant keeps its shape on BOTH boxes. The
  // price is that a box needs  2 x cap + mid_min <= width;  a height is therefore chosen per box so that holds, and PlaceBar REFUSES (BarPlace::refused, the
  // caller logs once) a box where it does not. The slant itself only occupies the outer ~quarter of a cap (about 9 units of 40 in the owner's picture, native
  // ~8 of 32): the rest of a cap is plain fill, so the text may lie over a cap's inner part as long as it clears the outer quarter by a few units.
  //   Regular one row : w 104, h 40 -> caps 40 + 40, middle 24 (text 89 advance wide: the lane's picture C shows it clear of the slant).
  //   Regular two rows: w 104, h 48 -> caps 48 + 48, middle  8. (reg_h2 was 64 for the 46 one: in proportion it would be 56, whose caps (112) are wider than the
  //                     box; 48 is the tallest the 104 allows with mid_min; the rows sit at +-row_dy 12, inside a +-24 box.) [guess: tune on the DOWNLOADED tab]
  //   Small           : w 58, h 24 -> caps 24 + 24, middle 10 (the old 30 would give caps 60 > 58). The slant takes the outer ~6 units; "LT W RT" at
  //                     small_scale is ~47 advance wide / ~40 visible, so its left edge lies ~9 units in: ~3 units clear of the slant, over the caps' inner
  //                     part (~18 of their 24 units, advance width) which is plain fill. The room beside a name bar is ~65 units and the box ends ~9 units from the
  //                     screen's edge [first look: x 1158-1262 of 1280]: width, centre and so the right edge are UNCHANGED (58 at small_cx 147), only 6 units lower.
  float reg_h = 40;      // Regular, one row [run 13 measured 46; Jarrod chose variant C: 40]
  float reg_h2 = 48;     // Regular, two rows (DOWNLOADED tab with a page provider): see the rule above [guess]
  float reg_cx = 112.5f; // Regular: box centre, outward from the bar centre [measured: run 13 P1 centre 69.5 vs group x 182; run 9 P2 570.0 vs 458 -> 112]
  float reg_cy = -49;    // Regular: box centre y from the bar centre line [measured: y 114 vs group y 163]
  float small_w = 58;    // Small box width [measured run 12: x 576 .. 634]
  float small_h = 24;    // Small box height: the even-cap rule above (was 30 [measured run 12: ul_h 93 / 50 x 16])
  float small_cx = 147;  // Small: box centre, outward [measured run 12: 605 vs group x 458]
  float small_cy = 0;    // Small: level with the bar [measured run 12]

  // Text: centred in the box (text x = box centre), absolute scale.
  float text_scale = 1.035f;   // [measured run 13: font002_name 0.9 x row2_scale 1.15]
  float text_dy = 0;           // a single row: from the box centre [measured: vertically centred]
  float row_dy = 12;           // two rows: the top row is this far above the centre, the bottom row this far below [guess]
  float row1_scale = 0.8f;     // the top row's scale [guess]
  float small_scale = 0.81f;   // the Small box's text [measured run 12: 0.9 x 0.9]

  // The underline under the current letter: a copy of src_ul, centred at (the current letter's x, bottom row y + ul_dy).
  float ul_w = 12;    // [guess]
  float ul_h = 2;     // [note: "3 px under it" in the 1280 space = 1.5-2 here]
  float ul_dy = 10;   // from the bottom row's centre [run 144, Jarrod 2026-11-24: "could the red line be a little closer to the letter": was 13]
  float small_ul_dy = 7;  // (7 since 2026-11-24 10:58, Jarrod: "nudge the red line up a tiny bit": at 8 it sat on the bottom rim) the same for the Small box: it is 24 high (+-12), so 10 would put the 2-high line on the bottom rim (10 +- 1 vs the rim's inner edge ~10.8);
                          // 8 keeps ~1.8 clear of the rim and ~1.5 under the glyphs (Regular: 10 in +-20) [guess]
  float ul_r = 0.8235f, ul_g = 0.0196f, ul_b = 0.0196f, ul_a = 1.0f;  // the screen's red, written into the rectangle's own colour (it REPLACES, a multiplier cannot) [file #1263]

  // Glyph advances in layout units at text_scale 1 (the underline's x only; the text itself is laid out by the game).
  float adv_icon = 16.5f;    // an LT / RT glyph [guess, calibrated: the lane's "LT M R S RT" fills ~92 units at 1.035]
  float adv_space = 5.5f;    // [guess, same]
  float adv_letter = 10.5f;  // an average capital; LetterAdvance() narrows I / J and widens M / W [guess]

  // Multipliers written into the box pieces' colour (+148..+160): 1 = as drawn by the game's art.
  float box_r = 1, box_g = 1, box_b = 1, box_a = 1;
  // The box pieces: the caps keep their native width (0) or scale with the height so the slant keeps its shape (1); the middle runs this far under each cap.
  float cap_sx = 0.5f;       // the caps' x scale when cap_keep_shape is 0 (the old uneven stretch: x 0.5 against y 1.44, a jagged diagonal) [kept as an option]
  float cap_keep_shape = 1;  // 1 = the caps' x scale follows the height (cap_sx ignored) so the slant keeps its shape [run 144, Jarrod 2026-11-24 "the slanted sides are a
                             //  bit aliased looking" and variant C]: the box rule above makes both boxes fit
  float mid_min = 6;         // the narrowest middle PlaceBar accepts between the two caps; less and it REFUSES the box (BarPlace::refused) [guess]
  float c_overlap = 2;       // [guess: hides a seam if the drawn middle is a little narrower than its size field]
  float flip_slant = 0;      // 1 = the right side's caps are mirrored (swapped, x scale negated) [U: a negative scale may not draw; the game's own right bar is NOT mirrored: both bars
                             // use the same L piece and the R piece = L rotated 180 degrees (file #41 +32 = 180)]
  float use_pills = 0;       // 1 = LT / RT come from the game's own font000_LT / _RT elements instead of characters in the string [off: the owner decides]
  float pill_dx = 12;        // pills: centre this far inside the box's left / right edge [guess]
  float pill_scale = 0.6f;   // [file #159: 0.6]
};

// "k=v,k=v": the keys are the field names. Numbers must be whole numbers (strtof, nothing left over, finite); the string keys take the text up to the next
// comma. Unknown keys and bad numbers are reported in *err (one line each, ';'-joined) and skipped; the good pairs are applied. Returns how many were.
inline int ParseLayout(const std::string& spec, BoxLayout& L, std::string* err = nullptr) {
  struct NumKey { const char* name; float BoxLayout::*p; };
  struct StrKey { const char* name; std::string BoxLayout::*p; };
  static const NumKey nums[] = {
      {"reg_w", &BoxLayout::reg_w}, {"reg_h", &BoxLayout::reg_h}, {"reg_h2", &BoxLayout::reg_h2}, {"reg_cx", &BoxLayout::reg_cx}, {"reg_cy", &BoxLayout::reg_cy},
      {"small_w", &BoxLayout::small_w}, {"small_h", &BoxLayout::small_h}, {"small_cx", &BoxLayout::small_cx}, {"small_cy", &BoxLayout::small_cy},
      {"text_scale", &BoxLayout::text_scale}, {"text_dy", &BoxLayout::text_dy}, {"row_dy", &BoxLayout::row_dy}, {"row1_scale", &BoxLayout::row1_scale},
      {"small_scale", &BoxLayout::small_scale}, {"small_ul_dy", &BoxLayout::small_ul_dy}, {"mid_min", &BoxLayout::mid_min}, {"ul_w", &BoxLayout::ul_w}, {"ul_h", &BoxLayout::ul_h}, {"ul_dy", &BoxLayout::ul_dy}, {"ul_r", &BoxLayout::ul_r},
      {"ul_g", &BoxLayout::ul_g}, {"ul_b", &BoxLayout::ul_b}, {"ul_a", &BoxLayout::ul_a}, {"adv_icon", &BoxLayout::adv_icon}, {"adv_space", &BoxLayout::adv_space},
      {"adv_letter", &BoxLayout::adv_letter}, {"box_r", &BoxLayout::box_r}, {"box_g", &BoxLayout::box_g}, {"box_b", &BoxLayout::box_b}, {"box_a", &BoxLayout::box_a},
      {"cap_sx", &BoxLayout::cap_sx}, {"cap_keep_shape", &BoxLayout::cap_keep_shape}, {"c_overlap", &BoxLayout::c_overlap}, {"flip_slant", &BoxLayout::flip_slant}, {"use_pills", &BoxLayout::use_pills},
      {"pill_dx", &BoxLayout::pill_dx}, {"pill_scale", &BoxLayout::pill_scale}};
  static const StrKey strs[] = {{"anchor", &BoxLayout::anchor}, {"box_src", &BoxLayout::box_src}, {"src_text", &BoxLayout::src_text}, {"src_ul", &BoxLayout::src_ul},
                                {"pill_l", &BoxLayout::pill_l}, {"pill_r", &BoxLayout::pill_r}, {"dim", &BoxLayout::dim}};
  auto trim = [](std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '"')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '"')) --b;
    return s.substr(a, b - a);
  };
  int applied = 0;
  std::string problems;
  auto bad = [&](const std::string& m) { problems += (problems.empty() ? "" : "; ") + m; };
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t comma = spec.find(',', pos);
    if (comma == std::string::npos) comma = spec.size();
    const std::string item = trim(spec.substr(pos, comma - pos));
    pos = comma + 1;
    if (item.empty()) continue;
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      bad("no '=' in '" + item + "'");
      continue;
    }
    const std::string key = trim(item.substr(0, eq)), val = trim(item.substr(eq + 1));
    bool done = false;
    for (const NumKey& n : nums) {
      if (key != n.name) continue;
      done = true;
      char* end = nullptr;
      const float f = std::strtof(val.c_str(), &end);
      if (val.empty() || end == val.c_str() || *end != 0 || !std::isfinite(f)) bad("bad number '" + val + "' for " + key);
      else {
        L.*(n.p) = f;
        ++applied;
      }
    }
    for (const StrKey& s : strs) {
      if (key != s.name) continue;
      done = true;
      if (val.empty()) bad("empty value for " + key);
      else {
        L.*(s.p) = val;
        ++applied;
      }
    }
    if (!done) bad("unknown key '" + key + "'");
  }
  if (err) *err = problems;
  return applied;
}

// ---------------------------------------------------------------------------------------------------------------- the current letter's place
// The advance of one letter in layout units at text_scale 1: I narrow, J a little narrow, M / W wide [guess: capitals of an italic face]. Used for the underline only.
inline float LetterAdvance(const BoxLayout& L, char c) {
  if (c == 'I') return L.adv_letter * 0.55f;
  if (c == 'J') return L.adv_letter * 0.8f;
  if (c == 'M' || c == 'W') return L.adv_letter * 1.3f;
  return L.adv_letter;
}

// The glyph advances of a drawn row (already multiplied by the text scale) and which glyph is the current letter.
struct GlyphRun {
  std::vector<float> adv;
  int cur = -1;
};
// x of the current glyph's centre relative to the centre of the whole run (the game centres the string): sum of the advances before it + half its own - half the total.
inline float UnderlineOffset(const GlyphRun& r) {
  if (r.cur < 0 || r.cur >= int(r.adv.size())) return 0;
  float before = 0, total = 0;
  for (size_t i = 0; i < r.adv.size(); ++i) {
    if (int(i) < r.cur) before += r.adv[i];
    total += r.adv[i];
  }
  return before + r.adv[size_t(r.cur)] / 2 - total / 2;
}
// The run of StripRow2's string: [LT sp] [prev sp] CUR [sp next] [sp RT], tokens as StripRow2 writes them.
inline GlyphRun RunFor(const std::vector<char>& present, char cur, Box box, const BoxLayout& L, float scale, bool glyphs = true) {
  GlyphRun r;
  if (cur == 0 || box == Box::None) return r;
  char prev = 0, next = 0;
  if (box == Box::Regular) Neighbours(present, cur, prev, next);
  auto add = [&](float a) { r.adv.push_back(a * scale); };
  if (glyphs) {
    add(L.adv_icon);
    add(L.adv_space);
  }
  if (prev) {
    add(LetterAdvance(L, prev));
    add(L.adv_space);
  }
  r.cur = int(r.adv.size());
  add(LetterAdvance(L, cur));
  if (next) {
    add(L.adv_space);
    add(LetterAdvance(L, next));
  }
  if (glyphs) {
    add(L.adv_space);
    add(L.adv_icon);
  }
  return r;
}

// ---------------------------------------------------------------------------------------------------------------- placement
struct Placed {
  float cx = 0, cy = 0, w = 0, h = 0;  // the box's CENTRE and size, in the bar group's space
  float text_x = 0;                    // the text elements' x (centred in the box: = cx)
  float row1_y = 0, row2_y = 0;        // the two rows' y (centres)
  float row_scale1 = 1, row_scale2 = 1;
  float ul_x = 0, ul_y = 0;            // the underline's centre
  float pill_lx = 0, pill_rx = 0;      // the pills' centres (use_pills)
  bool two_rows = false;
  float x0() const { return cx - w / 2; }
  float y0() const { return cy - h / 2; }
};

// side 0 = left panel: the box lies on the left; side 2 (and the centre side's Small box): on the right. THE MIRROR: the same numbers with the x sign flipped about the
// bar's centre (the panels sit at x 182 / 458 of 640, mirror images about 320); the text order is untouched. cur_dx = UnderlineOffset of the row.
inline Placed PlaceBox(const BoxLayout& L, Box box, bool has_top_row, int side, float cur_dx) {
  Placed p;
  const float s = (side == 0) ? -1.0f : 1.0f;
  if (box == Box::Regular) {
    p.w = L.reg_w;
    p.two_rows = has_top_row;
    p.h = has_top_row ? L.reg_h2 : L.reg_h;
    p.cx = s * L.reg_cx;
    p.cy = L.reg_cy;
    p.row1_y = p.cy - L.row_dy;
    p.row2_y = has_top_row ? p.cy + L.row_dy : p.cy + L.text_dy;
    p.row_scale1 = L.row1_scale;
    p.row_scale2 = L.text_scale;
  } else {
    p.w = L.small_w;
    p.h = L.small_h;
    p.cx = s * L.small_cx;
    p.cy = L.small_cy;
    p.row2_y = p.cy + L.text_dy;
    p.row_scale2 = L.small_scale;
  }
  p.text_x = p.cx;
  p.ul_x = p.text_x + cur_dx;
  p.ul_y = p.row2_y + (box == Box::Small ? L.small_ul_dy : L.ul_dy);
  p.pill_lx = p.cx - p.w / 2 + L.pill_dx;
  p.pill_rx = p.cx + p.w / 2 - L.pill_dx;
  return p;
}

// The box pieces are CENTRE-anchored sprites: their x is the centre of the drawn picture, their drawn width = size field (+20) x scale (+12).
//   native sizes (drawn, in layout units): lw / rw = the end caps, cw = the middle, h = the height of the three.
//   Result: the centres of the two cap positions and the middle, and the factors to multiply each copy's own x / y scale by. The left cap's outer edge is at the box's
//   left, the right cap's at its right, the middle spans exactly between the caps (c_overlap further under each).
struct PieceSizes {
  float lw = 16, cw = 160, rw = 16, h = 16;
};
struct BarPlace {
  float xl = 0, xc = 0, xr = 0;           // centres of: the left cap position, the middle, the right cap position
  float sxl = 1, sxc = 1, sxr = 1;        // x scale factors (the caps: cap_sx, or the height's with cap_keep_shape; < 0 = mirrored)
  float sy = 1;                           // y scale factor of all three
  bool swap = false;                      // the copy of the R piece goes to the left position (flip_slant), the L piece to the right
  float left_cap = 0, right_cap = 0;      // the drawn widths of the two caps
  float mid_w = 0;                        // the drawn width of the middle
  bool refused = false;                   // the caps plus mid_min do not fit the box: the caller must NOT draw it (and logs once), overlapping caps are never drawn
};
inline BarPlace PlaceBar(const Placed& p, const PieceSizes& n, const BoxLayout& L, int side) {
  BarPlace b;
  b.swap = (L.flip_slant != 0.0f) && side == 2;
  b.sy = n.h > 0.5f ? p.h / n.h : 1.0f;
  const float capk = (L.cap_keep_shape != 0.0f) ? b.sy : L.cap_sx;
  const float lw = b.swap ? n.rw : n.lw, rw = b.swap ? n.lw : n.rw;
  b.left_cap = lw * capk;
  b.right_cap = rw * capk;
  b.refused = b.left_cap + b.right_cap + L.mid_min > p.w;
  const float x0 = p.x0();
  const float span = std::fmax(1.0f, p.w - b.left_cap - b.right_cap);
  b.mid_w = span + 2 * L.c_overlap;
  b.xl = x0 + b.left_cap / 2;
  b.xr = x0 + p.w - b.right_cap / 2;
  b.xc = x0 + b.left_cap + span / 2;
  b.sxl = b.swap ? -capk : capk;
  b.sxr = b.swap ? -capk : capk;
  b.sxc = n.cw > 0.5f ? b.mid_w / n.cw : 1.0f;
  return b;
}

}  // namespace wwe2k14::letter_ui
