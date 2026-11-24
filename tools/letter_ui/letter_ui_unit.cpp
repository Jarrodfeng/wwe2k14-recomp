// S110 LETTERUI: host unit for src\letter_ui_core.h (which box a panel gets; the two strip strings; the layout table, its override, the source lists; how a panel
// reaches its name bar; the box by centres; the mirror; the underline's x).
// Made-up layout entries only (side / kind numbers as the note's table reads them), no game data.
//   letter_ui_unit.exe     every check must PASS (exit 0; the last line is "PASS letter_ui_unit" or "FAIL letter_ui_unit").
// Built and run (with its planted twins) by tools\letter_ui\letter_ui_selftest.py.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "letter_jump_core.h"
#include "letter_ui_core.h"

namespace lu = wwe2k14::letter_ui;
namespace lj = wwe2k14::letter_jump;

static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_fail;
    std::printf("FAIL  %s\n", what.c_str());
  }
}
static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

struct Entry {
  lu::PanelRow rows[6];
  bool vs;
};
// kinds / sides per row; 14 = none
static Entry Make(std::initializer_list<std::pair<int, int>> rows, bool vs) {
  Entry e;
  int i = 0;
  for (auto& r : rows) {
    e.rows[i].kind = r.first;
    e.rows[i].side = r.second;
    ++i;
  }
  e.vs = vs;
  return e;
}
static const char* Name(lu::Box b) { return b == lu::Box::Regular ? "Regular" : b == lu::Box::Small ? "Small" : "None"; }
static void Expect(const char* what, const Entry& e, int k, lu::Box want) {
  const lu::Box got = lu::SizeFor(e.rows, k, e.vs);
  Check(got == want, std::string("SizeFor ") + what + " row " + std::to_string(k) + ": want " + Name(want) + " got " + Name(got));
}

static void SizeCases() {
  // one on one: 1 left + 1 right (+ 2 managers, kind 2)
  const Entry oneone = Make({{0, 0}, {0, 2}, {2, 0}, {2, 2}}, true);
  Expect("1v1", oneone, 0, lu::Box::Regular);
  Expect("1v1", oneone, 1, lu::Box::Regular);
  Expect("1v1 manager", oneone, 2, lu::Box::None);
  // handicap: the single is left, the others right
  const Entry onetwo = Make({{0, 0}, {0, 2}, {0, 2}}, true);
  Expect("1v2", onetwo, 0, lu::Box::Regular);
  Expect("1v2", onetwo, 1, lu::Box::Small);
  Expect("1v2", onetwo, 2, lu::Box::Small);
  const Entry onethree = Make({{0, 0}, {0, 2}, {0, 2}, {0, 2}}, true);
  Expect("1v3", onethree, 0, lu::Box::Regular);
  Expect("1v3", onethree, 3, lu::Box::Small);
  const Entry twothree = Make({{0, 0}, {0, 0}, {0, 2}, {0, 2}, {0, 2}}, true);
  Expect("2v3", twothree, 0, lu::Box::Small);
  Expect("2v3", twothree, 4, lu::Box::Small);
  const Entry tag = Make({{0, 0}, {0, 0}, {0, 2}, {0, 2}, {2, 0}, {2, 2}}, true);
  Expect("tag", tag, 0, lu::Box::Small);
  Expect("tag", tag, 3, lu::Box::Small);
  const Entry six = Make({{0, 0}, {0, 0}, {0, 0}, {0, 2}, {0, 2}, {0, 2}}, true);
  for (int k = 0; k < 6; ++k) Expect("6-man", six, k, lu::Box::Small);
  // triple threat: three men, no VS record (centre + left + right)
  const Entry threeway = Make({{0, 1}, {0, 0}, {0, 2}}, false);
  for (int k = 0; k < 3; ++k) Expect("triple threat", threeway, k, lu::Box::Small);
  // a lone centre panel WITH a versus record is still not Regular (the centre is never a "side")
  const Entry centre = Make({{0, 1}}, true);
  Expect("centre with vs", centre, 0, lu::Box::Small);
  // without a VS record a lone side is Small
  const Entry novs = Make({{0, 0}, {0, 2}}, false);
  Expect("1v1 without vs", novs, 0, lu::Box::Small);
  // referee layout: 1 + 1 + a referee row (kind 1)
  const Entry refl = Make({{0, 0}, {0, 2}, {1, 1}}, true);
  Expect("1v1 ref", refl, 0, lu::Box::Regular);
  Expect("referee row", refl, 2, lu::Box::None);
  Expect("empty row", refl, 4, lu::Box::None);
  Expect("out of range", refl, 7, lu::Box::None);
}

static std::vector<char> L(const std::string& s) { return std::vector<char>(s.begin(), s.end()); }
static const std::string kDim = "FFFFFF66";
static const std::string kLT = "\xEE\x80\x8C", kRT = "\xEE\x80\x8F", kLB = "\xEE\x80\x8B", kRB = "\xEE\x80\x8E";
static std::string D(char c) { return "<$COLOR #" + kDim + ">" + std::string(1, c) + "</COLOR>"; }

static void StripCases() {
  Check(lu::StripRow2(L("ABQRS"), 'R', lu::Box::Regular, kDim) == kLT + " " + D('Q') + " R " + D('S') + " " + kRT, "Regular strip: LT, dim prev, plain current, dim next, RT");
  Check(lu::StripRow2(L("ABQRS"), 'R', lu::Box::Small, kDim) == kLT + " R " + kRT, "Small strip: the current letter only");
  Check(lu::StripRow2(L("ABQRS"), 'A', lu::Box::Regular, kDim) == kLT + " " + D('S') + " A " + D('B') + " " + kRT, "wrap at the first letter: prev is the last");
  Check(lu::StripRow2(L("ABQRS"), 'S', lu::Box::Regular, kDim) == kLT + " " + D('R') + " S " + D('A') + " " + kRT, "wrap at the last letter: next is the first");
  Check(lu::StripRow2(L("#AB"), 'A', lu::Box::Regular, kDim) == kLT + " " + D('#') + " A " + D('B') + " " + kRT, "the symbol bucket sorts first");
  Check(lu::StripRow2(L("Q"), 'Q', lu::Box::Regular, kDim) == kLT + " Q " + kRT, "one letter present: no neighbours");
  Check(lu::StripRow2(L("QR"), 'Q', lu::Box::Regular, kDim) == kLT + " " + D('R') + " Q " + D('R') + " " + kRT, "two letters: both neighbours are the other one");
  Check(lu::StripRow2(L(""), 0, lu::Box::Regular, kDim).empty(), "no letter: no strip");
  Check(lu::StripRow2(L("AB"), 'A', lu::Box::None, kDim).empty(), "no box: no strip");
  Check(lu::StripRow2(L("AB"), 'M', lu::Box::Regular, kDim) == kLT + " " + D('B') + " M " + D('A') + " " + kRT, "a current letter that is not present (a locked row): neighbours by order");
  const std::string s = lu::StripRow2(L("ABQRS"), 'R', lu::Box::Regular, kDim);
  Check(s.find("R </COLOR>") == std::string::npos && s.find(">R<") == std::string::npos, "the current letter is never inside a colour tag");
  Check(kLT == "\xEE\x80\x8C" && std::string(lu::GlyphLT()) == "\xEE\x80\x8C" && std::string(lu::GlyphRT()) == "\xEE\x80\x8F", "LT / RT are U+E00C / U+E00F");
  Check(std::string(lu::GlyphLB()) == "\xEE\x80\x8B" && std::string(lu::GlyphRB()) == "\xEE\x80\x8E", "LB / RB are U+E00B / U+E00E");
  Check(lu::StripRow2(L("ABQRS"), 'R', lu::Box::Regular, kDim, false) == D('Q') + " R " + D('S'), "the pill option leaves the LT / RT characters out of the string");

  // the top row: with and without a page provider (pages = 0)
  Check(lu::StripRow1(true, 2, 5) == kLB + " PAGE 2 / 5 " + kRB, "top row: LB PAGE k / n RB");
  Check(lu::StripRow1(true, 0, 0).empty(), "top row: no provider (pages 0) draws nothing");
  Check(lu::StripRow1(false, 2, 5).empty(), "top row: not the DOWNLOADED tab draws nothing");
  Check(lu::StripRow1(true, 0, 5).empty(), "top row: page 0 draws nothing");

  // the strip and the jump agree: PresentLetters is the jump's own set
  std::vector<lj::Row> rows;
  const std::string names = "BQAB#R";
  for (size_t i = 0; i < names.size(); ++i) {
    lj::Row r;
    r.index = int(i);
    r.letter = names[i];
    r.selectable = i != 3;  // the second B is not selectable (does not change the set)
    rows.push_back(r);
  }
  rows[4].selectable = false;  // the '#' row cannot be rested on: not a letter the jump lands on
  Check(lj::PresentLetters(rows) == L("ABQR"), "PresentLetters: ascending, distinct, selectable only");
}

static void LayoutCases() {
  lu::BoxLayout b;
  std::string err;
  Check(lu::ParseLayout("reg_w=200,small_w=90.5", b, &err) == 2 && err.empty() && Near(b.reg_w, 200) && Near(b.small_w, 90.5f), "layout override: good pairs applied");
  b = lu::BoxLayout();
  Check(lu::ParseLayout("bogus=3,reg_w=150", b, &err) == 1 && Near(b.reg_w, 150) && err.find("bogus") != std::string::npos, "layout override: an unknown key is reported, the rest applied");
  b = lu::BoxLayout();
  const float before = b.reg_w;
  Check(lu::ParseLayout("reg_w=abc", b, &err) == 0 && Near(b.reg_w, before) && err.find("bad number") != std::string::npos, "layout override: a bad number is reported and not applied");
  Check(lu::ParseLayout("reg_w=12x", b, &err) == 0 && err.find("bad number") != std::string::npos, "layout override: trailing junk is a bad number");
  Check(lu::ParseLayout("reg_w=inf", b, &err) == 0, "layout override: a non-finite number is refused");
  Check(lu::ParseLayout("", b, &err) == 0 && err.empty(), "layout override: empty is nothing");
  Check(lu::ParseLayout("src_ul=panel_waku01_L, dim=FF0000FF", b, &err) == 2 && b.src_ul == "panel_waku01_L" && b.dim == "FF0000FF", "layout override: string keys (spaces trimmed)");
  Check(lu::ParseLayout("reg_w", b, &err) == 0 && !err.empty(), "layout override: a pair without '=' is reported");
  Check(lu::ParseLayout("\"reg_cy=-170\"", b, &err) == 1 && Near(b.reg_cy, -170), "layout override: quotes and a negative number");
  Check(lu::ParseLayout("box_src=panel_waku01,use_pills=1", b, &err) == 2 && b.box_src == "panel_waku01" && b.use_pills == 1.0f, "layout override: the box art list and the pill option switch by key");
}

// the source lists: the first base name that resolves wins
static void SourceCases() {
  const auto bases = lu::SplitList("panel_waku02|panel_waku01");
  Check(bases.size() == 2 && bases[0] == "panel_waku02" && bases[1] == "panel_waku01", "SplitList: two names in order");
  Check(lu::SplitList("a||b|").size() == 2 && lu::SplitList("").empty(), "SplitList: empty items dropped");
  Check(lu::FirstResolving(bases, [](const std::string&) { return true; }) == 0, "source list: both resolve -> the first wins");
  Check(lu::FirstResolving(bases, [](const std::string& s) { return s == "panel_waku01"; }) == 1, "source list: only the second resolves -> the second");
  Check(lu::FirstResolving(bases, [](const std::string&) { return false; }) == -1, "source list: none resolves -> -1");
  lu::BoxLayout d;
  Check(lu::SplitList(d.box_src).size() == 2 && lu::SplitList(d.box_src)[0] == "panel_waku02", "the default box art is the attire box (waku02) with the name bar (waku01) behind it");
}

// how a panel reaches its name bar: group 33 + k at the index of the panel's +37020 word (made-up tables of the game's shape)
static void BarCases() {
  const uint32_t table[12] = {119, 33, 120, 34, 121, 35, 122, 36, 123, 37, 124, 38};  // the game's table: {record, group} per slot
  Check(lu::GroupFromTable(table, 0) == 33 && lu::GroupFromTable(table, 1) == 34 && lu::GroupFromTable(table, 5) == 38 && lu::GroupFromTable(table, 6) == -1, "the group is the SECOND word of the table entry (33 + k), not the record word");
  const int groups[6] = {33, 34, 35, 36, 37, 38};
  // one on one: P1 left, P2 right
  Check(lu::BarSlotFor(0, false) == 6 && lu::BarSlotFor(2, false) == 7 && lu::BarSlotFor(1, false) == 8 && lu::BarSlotFor(1, true) == 9, "bar slot by side: 6 left, 7 right, 8 centre, 9 centre in list mode");
  const lu::PanelRow oneone[6] = {{0, 0}, {0, 2}, {2, 0}, {2, 2}};
  Check(lu::BarElemOf(groups, 0, lu::BarSlotFor(oneone[0].side, false)) == lu::ElemRef{33, 6}, "one on one k0: group 33, element 6 (CharSet_02L)");
  Check(lu::BarElemOf(groups, 1, lu::BarSlotFor(oneone[1].side, false)) == lu::ElemRef{34, 7}, "one on one k1: group 34, element 7 (CharSet_02R)");
  // 1 on 2: k0 left, k1 / k2 right
  const lu::PanelRow onetwo[6] = {{0, 0}, {0, 2}, {0, 2}};
  Check(lu::BarElemOf(groups, 2, lu::BarSlotFor(onetwo[2].side, false)) == lu::ElemRef{35, 7}, "1 on 2 k2: group 35, element 7 (a right panel)");
  Check(!(lu::BarElemOf(groups, 1, 7) == lu::BarElemOf(groups, 2, 7)), "two panels of one side have two different bar group elements (the children are shared, the group elements are not)");
  // 6-man: 3 left, 3 right
  const lu::PanelRow six[6] = {{0, 0}, {0, 0}, {0, 0}, {0, 2}, {0, 2}, {0, 2}};
  bool ok = true;
  for (int k = 0; k < 6; ++k) ok = ok && lu::BarElemOf(groups, k, lu::BarSlotFor(six[k].side, false)) == lu::ElemRef{33 + k, k < 3 ? 6 : 7};
  Check(ok, "6-man: panel k -> group 33 + k, element 6 for the left three and 7 for the right three");
  // triple threat: the centre panel in list mode
  Check(lu::BarElemOf(groups, 0, lu::BarSlotFor(1, true)) == lu::ElemRef{33, 9}, "list mode centre panel: element 9 (CharSet_02CC)");
  Check(lu::BarElemOf(groups, 6, 6).group == -1 && lu::BarElemOf(groups, -1, 6).group == -1 && lu::BarElemOf(groups, 0, -1).group == -1, "out of range: no bar");
}

// where the current letter's centre lies in the centred string: from the advances
static void UnderlineCases() {
  lu::GlyphRun r;
  r.adv = {10, 5, 8, 5, 6, 5, 8, 5, 10};  // LT sp prev sp CUR sp next sp RT
  r.cur = 4;
  Check(Near(lu::UnderlineOffset(r), 0), "underline x: a symmetric string has its current letter at the centre");
  r.adv = {10, 5, 14, 5, 6, 5, 8, 5, 10};  // a wide previous letter: the current one lies to the right of the centre
  Check(Near(lu::UnderlineOffset(r), 3.0f), "underline x: a wider previous letter moves the current one right of the centre (before 34 + half 3 - half of 68)");
  r.adv = {10, 5, 6, 5, 8, 5, 10};  // LT sp CUR sp next sp RT
  r.cur = 2;
  Check(Near(lu::UnderlineOffset(r), 10 + 5 + 3 - 49.0f / 2), "underline x: sum before + half itself - half the total");
  r.cur = -1;
  Check(Near(lu::UnderlineOffset(r), 0), "underline x: no current glyph -> 0");
  r.cur = 9;
  Check(Near(lu::UnderlineOffset(r), 0), "underline x: an index past the run -> 0");
  // the run of the real strip: symmetric widths for the Small box, any letter
  lu::BoxLayout d;
  const lu::GlyphRun small = lu::RunFor(L("ABQRS"), 'R', lu::Box::Small, d, 1.0f);
  Check(small.adv.size() == 5 && small.cur == 2 && Near(lu::UnderlineOffset(small), 0), "the Small strip's run: LT sp R sp RT, the letter at the centre");
  const lu::GlyphRun reg = lu::RunFor(L("ABQRS"), 'R', lu::Box::Regular, d, 1.0f);
  Check(reg.adv.size() == 9 && reg.cur == 4, "the Regular strip's run: nine tokens, the current one fifth");
  const lu::GlyphRun regw = lu::RunFor(L("ABMRW"), 'R', lu::Box::Regular, d, 1.0f);  // prev M wide, next W wide: still symmetric
  Check(Near(lu::UnderlineOffset(regw), 0), "the Regular run with equally wide neighbours is centred");
  const lu::GlyphRun regi = lu::RunFor(L("ABIRW"), 'R', lu::Box::Regular, d, 1.0f);  // prev I narrow, next W wide: the letter moves left
  Check(lu::UnderlineOffset(regi) < -0.5f, "a narrow previous letter and a wide next one: the current letter lies left of the centre");
  const lu::GlyphRun scaled = lu::RunFor(L("ABQRS"), 'R', lu::Box::Regular, d, 2.0f);
  Check(Near(scaled.adv[0], 2 * d.adv_icon), "the run's advances scale with the text scale");
  const lu::GlyphRun noglyph = lu::RunFor(L("ABQRS"), 'R', lu::Box::Regular, d, 1.0f, false);
  Check(noglyph.adv.size() == 5 && noglyph.cur == 2, "without the glyphs (pills) the run has no icon tokens");
  Check(lu::RunFor(L("A"), 0, lu::Box::Regular, d, 1.0f).adv.empty(), "no letter: an empty run");
}

static float AbsX(float group_x, float rel) { return group_x + rel; }

static void PlaceCases() {
  lu::BoxLayout d;
  // the mirror about the bar's centre = about x = 320: the panels sit at x 182 and 458
  const lu::Placed rl = lu::PlaceBox(d, lu::Box::Regular, true, 0, 3.0f);
  const lu::Placed rr = lu::PlaceBox(d, lu::Box::Regular, true, 2, 3.0f);
  Check(Near(AbsX(182, rl.cx) + AbsX(458, rr.cx), 640.0f), "mirror (Regular): the two boxes' centres are mirror images about x = 320");
  Check(rl.cx < 0 && rr.cx > 0, "mirror (Regular): the left panel's box is on its left, the right panel's on its right");
  Check(Near(rl.cy, rr.cy) && Near(rl.w, rr.w) && Near(rl.h, rr.h), "mirror (Regular): same height and size");
  Check(Near(rl.text_x - rl.cx, 0) && Near(rr.text_x - rr.cx, 0), "the text is centred in its box (no offset between the box and the text)");
  Check(Near(rl.row2_y - rl.cy, rr.row2_y - rr.cy) && Near(rl.row1_y - rl.cy, rr.row1_y - rr.cy), "mirror (Regular): the rows sit at the same place inside the box (the text order never changes)");
  Check(Near(rl.cx, -d.reg_cx) && Near(rl.cy, d.reg_cy) && Near(rl.w, d.reg_w), "Regular box: centre and size from the table");
  const lu::Placed sl = lu::PlaceBox(d, lu::Box::Small, false, 0, 0);
  const lu::Placed sr = lu::PlaceBox(d, lu::Box::Small, false, 2, 0);
  Check(Near(AbsX(182, sl.cx) + AbsX(458, sr.cx), 640.0f) && Near(sl.cx, -d.small_cx), "mirror (Small): the two boxes are mirror images about x = 320");
  const lu::Placed sc = lu::PlaceBox(d, lu::Box::Small, false, 1, 0);
  Check(sc.cx > 0 && Near(sc.cx, d.small_cx), "the centre panel's Small box goes to the right of its bar");
  Check(Near(lu::PlaceBox(d, lu::Box::Regular, false, 0, 0).h, d.reg_h) && Near(rl.h, d.reg_h2), "the Regular box shrinks to one row without a top row");
  Check(!lu::PlaceBox(d, lu::Box::Regular, false, 0, 0).two_rows && rl.two_rows, "the two_rows flag follows the top row");
  Check(Near(rl.row2_y, rl.cy + d.row_dy) && Near(rl.row1_y, rl.cy - d.row_dy), "two rows: one above the box's centre, one below");
  Check(Near(lu::PlaceBox(d, lu::Box::Regular, false, 0, 0).row2_y, d.reg_cy + d.text_dy), "one row: centred on the box");
  Check(Near(rl.ul_y, rl.row2_y + d.ul_dy) && Near(rl.ul_x, rl.text_x + 3.0f), "the underline sits under the current letter: the text centre plus the letter's offset");
  Check(Near(rl.x0(), rl.cx - rl.w / 2) && Near(rl.y0(), rl.cy - rl.h / 2), "the box's corner is derived from its centre");

  // the three box pieces, CENTRE-anchored: caps meet the middle at any box width (c_overlap 0: no gap, no overlap)
  lu::BoxLayout e;
  e.c_overlap = 0;
  e.cap_keep_shape = 0;  // these cases are the OLD uneven cap option (cap_sx); the even-cap default has its own cases in EvenCapCases
  const lu::PieceSizes n;  // 16 / 160 / 16 high 16 (the name bar's own pieces)
  for (float w : {58.0f, 104.0f, 220.0f}) {
    e.reg_w = w;
    const lu::Placed p = lu::PlaceBox(e, lu::Box::Regular, false, 0, 0);
    const lu::BarPlace b = lu::PlaceBar(p, n, e, 0);
    const float left_cap_right = b.xl + b.left_cap / 2, mid_left = b.xc - b.mid_w / 2, mid_right = b.xc + b.mid_w / 2, right_cap_left = b.xr - b.right_cap / 2;
    Check(Near(b.xl - b.left_cap / 2, p.x0()) && Near(b.xr + b.right_cap / 2, p.x0() + p.w), "box pieces w=" + std::to_string(int(w)) + ": the caps' outer edges are the box's edges");
    Check(Near(left_cap_right, mid_left) && Near(mid_right, right_cap_left), "box pieces w=" + std::to_string(int(w)) + ": the caps meet the middle (no gap, no overlap)");
    Check(Near(b.xc, p.cx), "box pieces w=" + std::to_string(int(w)) + ": the middle is centred on the box");
    Check(Near(b.sy, p.h / n.h), "box pieces w=" + std::to_string(int(w)) + ": y stretch = box height / native height");
  }
  // sizes other than 16: the real native sizes are used (the attire box: caps 32, middle 192, height 32)
  lu::PieceSizes att;
  att.lw = att.rw = 32, att.cw = 192, att.h = 32;
  e.reg_w = 104;
  const lu::Placed pa = lu::PlaceBox(e, lu::Box::Regular, false, 2, 0);
  const lu::BarPlace ba = lu::PlaceBar(pa, att, e, 2);
  Check(Near(ba.left_cap, 32 * e.cap_sx) && Near(ba.mid_w, pa.w - 2 * 32 * e.cap_sx) && Near(ba.sxc * att.cw, ba.mid_w), "box pieces: native sizes read from the elements drive the caps and the middle's stretch");
  e.cap_keep_shape = 1;
  const lu::BarPlace bk = lu::PlaceBar(pa, att, e, 2);
  Check(Near(bk.sxl, bk.sy) && Near(bk.left_cap, 32 * bk.sy), "box pieces: cap_keep_shape makes the caps follow the height");
  e.cap_keep_shape = 0;
  // the c_overlap runs under both caps
  e.c_overlap = 3;
  const lu::BarPlace bo = lu::PlaceBar(pa, att, e, 2);
  Check(Near(bo.mid_w, pa.w - 2 * 32 * e.cap_sx + 6), "box pieces: the middle runs c_overlap under each cap");
  e.c_overlap = 0;
  // the flip option: the right side's caps are swapped and negated; the left side never
  e.flip_slant = 1;
  const lu::BarPlace bf = lu::PlaceBar(pa, att, e, 2), bl = lu::PlaceBar(lu::PlaceBox(e, lu::Box::Regular, false, 0, 0), att, e, 0);
  Check(bf.swap && bf.sxl < 0 && bf.sxr < 0 && !bl.swap && bl.sxl > 0, "flip_slant: the right side's caps are swapped and mirrored, the left side's are not");
  // a tiny box never gets a negative span, and is refused
  e.flip_slant = 0;
  e.reg_w = 10;
  const lu::BarPlace bt = lu::PlaceBar(lu::PlaceBox(e, lu::Box::Regular, false, 0, 0), att, e, 0);
  Check(bt.mid_w >= 1.0f && bt.refused, "a box narrower than its caps still gets a positive middle (and is refused)");
}

// THE BOX RULE: the caps scale evenly with the height on both boxes (the defaults), and fit with a middle between them; an impossible box is refused.
static void EvenCapCases() {
  lu::BoxLayout d;
  Check(d.cap_keep_shape != 0.0f && Near(d.reg_h, 40) && Near(d.ul_dy, 10), "the defaults are the owner's variant C: even caps, Regular 40 high, the line 10 under the row");
  lu::BoxLayout e;
  e.c_overlap = 0;  // exact meeting
  lu::PieceSizes att;  // the attire box art as the game reads it: caps 32, middle 192, height 32 (read from the elements; nothing below hard-codes 32)
  att.lw = att.rw = 32, att.cw = 192, att.h = 32;
  struct Case { const char* what; lu::Box box; bool top; float h; };
  const Case cases[] = {{"Regular one row", lu::Box::Regular, false, e.reg_h}, {"Regular two rows", lu::Box::Regular, true, e.reg_h2}, {"Small", lu::Box::Small, false, e.small_h}};
  for (const Case& c : cases)
    for (int side : {0, 2}) {
      const std::string w = std::string(c.what) + " side " + std::to_string(side) + ": ";
      const lu::Placed p = lu::PlaceBox(e, c.box, c.top, side, 0);
      const lu::BarPlace b = lu::PlaceBar(p, att, e, side);
      Check(Near(p.h, c.h) && !b.refused, w + "the box fits its two even caps and a middle (not refused)");
      Check(Near(b.sxl, b.sy) && Near(b.sxr, b.sy) && Near(b.left_cap, att.lw * p.h / att.h) && Near(b.right_cap, att.rw * p.h / att.h), w + "the caps are scaled evenly with the height (x scale = y scale)");
      Check(Near(b.xl - b.left_cap / 2, p.x0()) && Near(b.xr + b.right_cap / 2, p.x0() + p.w), w + "the caps' outer edges are the box's edges");
      Check(Near(b.xl + b.left_cap / 2, b.xc - b.mid_w / 2) && Near(b.xc + b.mid_w / 2, b.xr - b.right_cap / 2), w + "the caps meet the middle (no gap, no overlap)");
      Check(Near(b.xc, p.cx) && Near(b.left_cap + b.mid_w + b.right_cap, p.w) && b.mid_w >= e.mid_min, w + "the middle is centred on the box and at least mid_min wide");
    }
  // Regular one row: the cap is native cap width x 40 / native height
  const lu::Placed rp = lu::PlaceBox(e, lu::Box::Regular, false, 0, 0);
  Check(Near(lu::PlaceBar(rp, att, e, 0).left_cap, att.lw * 40 / att.h), "Regular at reg_h 40: each cap is the native cap width x 40 / the native height");
  // Small: its numbers (the room beside a name bar is about 65 units; the box ended 176 units from the bar's centre, ~9 from the screen's edge)
  const lu::Placed sp = lu::PlaceBox(e, lu::Box::Small, false, 2, 0);
  Check(Near(sp.w, 58) && Near(sp.h, 24) && sp.w <= 65 && sp.cx + sp.w / 2 <= 176.01f, "Small: 58 x 24, inside the ~65 units of room and ending no further out than before (176 from the bar's centre)");
  const lu::BarPlace sb = lu::PlaceBar(sp, att, e, 2);
  Check(Near(sb.left_cap, 24) && Near(sb.mid_w, 10), "Small: caps 24 + 24 and a middle of 10");
  float run = 0;
  for (float a : lu::RunFor(L("W"), 'W', lu::Box::Small, e, e.small_scale).adv) run += a;
  Check(run < sp.w && Near(sp.ul_y - sp.row2_y, e.small_ul_dy) && Near(rp.ul_y - rp.row2_y, e.ul_dy), "Small: \"LT W RT\" is narrower than the box; its line is small_ul_dy under the row, the Regular's ul_dy");
  // the refusal: caps wider than the box, or leaving less than mid_min
  lu::BoxLayout bad = e;
  bad.small_h = 30;  // the old Small height at an even scale: caps 30 + 30 > 58
  const lu::BarPlace bb = lu::PlaceBar(lu::PlaceBox(bad, lu::Box::Small, false, 0, 0), att, bad, 0);
  Check(bb.refused, "an even-cap box whose caps are wider than the box is refused (never drawn overlapping)");
  lu::BoxLayout edge = e;
  edge.reg_w = 2 * 40 + edge.mid_min;  // exactly caps + mid_min
  Check(!lu::PlaceBar(lu::PlaceBox(edge, lu::Box::Regular, false, 0, 0), att, edge, 0).refused, "caps + exactly mid_min fit: not refused");
  edge.reg_w -= 0.5f;
  Check(lu::PlaceBar(lu::PlaceBox(edge, lu::Box::Regular, false, 0, 0), att, edge, 0).refused, "caps + less than mid_min: refused");
  lu::BoxLayout old = e;
  old.reg_h2 = 64;  // the old two-row height: caps 64 + 64 > 104
  Check(lu::PlaceBar(lu::PlaceBox(old, lu::Box::Regular, true, 0, 0), att, old, 0).refused, "the old two-row height 64 at an even scale is refused");
}

int main() {
  SizeCases();
  StripCases();
  LayoutCases();
  SourceCases();
  BarCases();
  UnderlineCases();
  PlaceCases();
  EvenCapCases();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  std::printf("%s letter_ui_unit\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
