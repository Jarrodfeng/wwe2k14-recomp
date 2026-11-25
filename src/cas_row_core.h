// S110 CASROW: the pure part of the Create a Superstar row verb (src/cas_row.h; docs\research\s110\CASROW.md): the request, the editor's
// CURRENT top object (the one a pad press reaches now), the cursor plan, the message box answer and the route states. Std only (a reader
// functor stands in for guest memory), so tools/goto/cas_row_test.cpp drives it on planted objects. src/cas_pick.h uses FindTop too.
//
// The editor's objects, all read from the listings (CASROW.md claims):
//   - the page host H: H+948 the page, H+952 overlay A (an item page's container, vtable 0x821BE5C0, its panel at +936), H+956 / +1004
//     overlay B / pending (src/cas_goto.h).
//   - the parts panel P (vtable 0x821C08D8): its state switch P+992 (sub_833FA688: 0 / 8 the list, 2 the part's edit page S up (slot 50
//     sub_833FFAD8), 3 S in an edit stage (slot 51), 4 / 5 the placement / colour tool (slots 52 / 53, the tool at P+1000), 10 a message
//     box (slot 54 sub_833FA940)); S at P+996.
//   - the edit page class S (vtable 0x821C1580, ctor sub_834070A0, mixin S+28 vtable 0x821C14A8): the part's edit page (kind S+948 = 1),
//     the LOGOS page S2 (kind 2) and the side page (kind 3) are this one class. Its update sub_83407F98 switches on S+952: 0 its rows;
//     1 / 2 an S-class child at S+1088 (sub_83407890); 3 a stage object at S+1088 (sub_834079D0: the logo list P2 is a parts panel); 4 the
//     child at S+1088 in its own stage (sub_83405E00); 5 the stage object at S+1092 (sub_83407B78, row 6293); 6 / 7 S waits on the
//     message box (sub_83405FA8 / sub_83406020); 8 / 9 a stage change (case 9 builds stage S+1144 by sub_83407E08).
//   - the message box: the one manager [0x839294BC] (docs/research/s110/MSGBOX.md): +16 1 opening, 2 waiting for the answer, 3 answered,
//     4 closing, 5 idle; +1792 the text id; +1800 the kind (0 / 1 OK, 2 / 3 YES / NO, 4 / 5 a list, 6 / 7 no input); +64 the YES / NO
//     column (0 YES, 1 NO); its input mixin at box+28 (vtable 0x82047470).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "cas_goto_core.h"  // kListVtable, kItemPageVtable, kContainerVtable, kContainerInner, kPartsPanelVtable

namespace wwe2k14::cas_row {

using cas_goto::kContainerInner;
using cas_goto::kContainerVtable;
using cas_goto::kItemPageVtable;
using cas_goto::kListVtable;
using cas_goto::kPartsPanelVtable;

constexpr uint32_t kEditPageVtable = 0x821C1580, kEditMixinVtable = 0x821C14A8;  // S (sub_834070A0) and its input mixin S+28
constexpr uint32_t kBoxPtr = 0x839294BC, kBoxMixinVtable = 0x82047470;          // the message box manager and its mixin box+28
constexpr uint32_t kHPage = 948, kHOverlayA = 952, kHOverlayB = 956, kHOverlayPending = 1004;
constexpr uint32_t kState = 16, kMixin = 28;                                    // every page: +16 (2 idle, 3 / 4 closed, 5 done), mixin +28
constexpr uint32_t kPState = 992, kPEdit = 996, kPTool = 1000;                  // the parts panel's switch, S, the tool
constexpr uint32_t kSKind = 948, kSStage = 952, kSChild = 1088, kSStageObj = 1092, kSStageNext = 1144;  // S
constexpr uint32_t kBoxState = 16, kBoxText = 1792, kBoxKind = 1800, kBoxColumn = 64, kBoxAEnable = 1856;  // the box (MSGBOX claims 4 / 5 / 8)
constexpr uint32_t kMaxDepth = 8;

// ---- The request (the cvar wwe2k14_cas_row) ----
enum class Ask : uint32_t { kNone = 0, kRow, kBack, kYes, kNo, kOk };
struct Request {
  Ask ask = Ask::kNone;
  uint32_t label = 0;  // kRow: the game's row label id (6340 LOGOS, 6678 DESIGNS, 6315 ACCEPT ...)
};
// The published request code (cas_row_label): the label for a row; 1 back, 2 yes, 3 no, 4 ok (no row label is below 1000).
inline uint32_t RequestCode(const Request& q) {
  switch (q.ask) {
    case Ask::kRow: return q.label;
    case Ask::kBack: return 1;
    case Ask::kYes: return 2;
    case Ask::kNo: return 3;
    case Ask::kOk: return 4;
    default: return 0;
  }
}
// "" = off; a label id (digits, 1000..99999), or back / yes / no / ok. Returns "" or why not.
inline std::string ParseRequest(const std::string& s, Request& q) {
  q = Request{};
  const Ask word = s == "back" ? Ask::kBack : s == "yes" ? Ask::kYes : s == "no" ? Ask::kNo : s == "ok" ? Ask::kOk : Ask::kNone;
  if (word != Ask::kNone) {
    q.ask = word;
    return "";
  }
  if (s.empty() || s.size() > 5) return "give a row label id (e.g. 6340 LOGOS), or back / yes / no / ok";
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return "'" + s + "' is neither a row label id (digits only) nor back / yes / no / ok";
    v = v * 10 + uint32_t(c - '0');
  }
  if (v < 1000) return "'" + s + "' is not a text label id (the editor's row labels are 4-5 digit text ids, e.g. 6340 LOGOS)";
  q.ask = Ask::kRow;
  q.label = v;
  return "";
}

// ---- The top object: the one a pad press reaches now ----
enum class TopKind : uint32_t {
  kNone = 0,      // no editor page host
  kEditPage = 1,  // an S-class page (the part's edit page, the LOGOS page, the side page): rows by label (this verb)
  kPanel = 2,     // a parts panel (the item page's, or the logo list P2): items by number (src/cas_pick.h)
  kBox = 3,       // the message box (answered by this verb: yes / no / ok)
  kList = 4,      // a list page (src/cas_goto.h walks it by flow record)
  kItemRows = 5,  // an item page showing its own rows (no overlay; sub_833C4700, not read)
  kOther = 6,     // an object whose class is not read (named in `why`)
  kBusy = 7,      // something is still moving (`why`)
};
struct Top {
  TopKind kind = TopKind::kNone;
  uint32_t obj = 0;    // the object a press reaches now (its mixin is obj + 28)
  uint32_t owner = 0;  // what holds it: the container (the item page's panel), the panel (S), the parent S (S2, a stage object)
  uint32_t depth = 0;  // objects below the host's page
  std::string why;
};

inline std::string Hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}
inline Top Make(TopKind k, uint32_t obj, uint32_t owner, uint32_t depth, std::string why = "") {
  Top t;
  t.kind = k, t.obj = obj, t.owner = owner, t.depth = depth, t.why = std::move(why);
  return t;
}
inline bool Closing(uint32_t state16) { return state16 == 3 || state16 == 4 || state16 == 5; }

template <class R>
Top EditTop(R rd, uint32_t s, uint32_t owner, uint32_t depth);

// A parts panel and what it shows (its switch P+992).
template <class R>
Top PanelTop(R rd, uint32_t pn, uint32_t owner, uint32_t depth) {
  if (depth > kMaxDepth) return Make(TopKind::kOther, pn, owner, depth, "nested deeper than " + std::to_string(kMaxDepth));
  const uint32_t st16 = rd(pn + kState), st = rd(pn + kPState);
  if (Closing(st16)) return Make(TopKind::kBusy, pn, owner, depth, "the parts panel " + Hex(pn) + " is closing (+16 = " + std::to_string(st16) + ")");
  if (st == 0 || st == 8) return Make(TopKind::kPanel, pn, owner, depth);
  if (st == 2 || st == 3) {
    const uint32_t s = rd(pn + kPEdit);
    if (!s) return Make(TopKind::kBusy, pn, owner, depth, "the parts panel " + Hex(pn) + " is building the part's edit page (P+992 = " + std::to_string(st) + ", P+996 = 0)");
    if (rd(s) != kEditPageVtable)
      return Make(TopKind::kOther, s, pn, depth + 1, "the parts panel's P+996 " + Hex(s) + " has vtable " + Hex(rd(s)) + ", not the edit page " + Hex(kEditPageVtable));
    return EditTop(rd, s, pn, depth + 1);
  }
  if (st == 10) return Make(TopKind::kBusy, pn, owner, depth, "the parts panel " + Hex(pn) + " waits on its message box (P+992 = 10, slot 54 sub_833FA940)");
  if (st == 4 || st == 5) {
    const uint32_t tool = rd(pn + kPTool);
    return Make(TopKind::kOther, tool, pn, depth + 1,
                "the parts panel " + Hex(pn) + "'s " + (st == 4 ? "placement (move / rotate / size)" : "colour") + " tool (P+992 = " + std::to_string(st) +
                    ", P+1000 = " + Hex(tool) + ", vtable " + Hex(tool ? rd(tool) : 0) + "): not read");
  }
  return Make(TopKind::kOther, pn, owner, depth, "the parts panel " + Hex(pn) + " in state P+992 = " + std::to_string(st) + " (not read)");
}

// An S-class page and what it shows (its update's switch S+952).
template <class R>
Top EditTop(R rd, uint32_t s, uint32_t owner, uint32_t depth) {
  if (depth > kMaxDepth) return Make(TopKind::kOther, s, owner, depth, "nested deeper than " + std::to_string(kMaxDepth));
  const uint32_t st16 = rd(s + kState), st = rd(s + kSStage);
  const std::string me = "the edit page " + Hex(s) + " (kind " + std::to_string(rd(s + kSKind)) + ")";
  if (Closing(st16)) return Make(TopKind::kBusy, s, owner, depth, me + " is closing (+16 = " + std::to_string(st16) + ")");
  switch (st) {
    case 0: return Make(TopKind::kEditPage, s, owner, depth);
    case 1:
    case 2:
    case 4: {
      const uint32_t c = rd(s + kSChild);
      if (!c) return Make(TopKind::kBusy, s, owner, depth, me + " is building its child page (S+952 = " + std::to_string(st) + ", S+1088 = 0)");
      if (rd(c) == kEditPageVtable) return EditTop(rd, c, s, depth + 1);
      return Make(TopKind::kOther, c, s, depth + 1, me + "'s child " + Hex(c) + " (vtable " + Hex(rd(c)) + ", S+952 = " + std::to_string(st) + "): not read");
    }
    case 3: {
      const uint32_t c = rd(s + kSChild);
      if (!c) return Make(TopKind::kBusy, s, owner, depth, me + " is building its stage (S+952 = 3, S+1088 = 0)");
      if (rd(c) == kPartsPanelVtable) return PanelTop(rd, c, s, depth + 1);
      if (rd(c) == kEditPageVtable) return EditTop(rd, c, s, depth + 1);
      return Make(TopKind::kOther, c, s, depth + 1,
                  me + "'s stage " + std::to_string(rd(s + kSStageNext)) + " object " + Hex(c) + " (vtable " + Hex(rd(c)) + "): its class is not read");
    }
    case 5: {
      const uint32_t c = rd(s + kSStageObj);
      if (!c) return Make(TopKind::kBusy, s, owner, depth, me + " is building its stage (S+952 = 5, S+1092 = 0)");
      if (rd(c) == kPartsPanelVtable) return PanelTop(rd, c, s, depth + 1);
      return Make(TopKind::kOther, c, s, depth + 1, me + "'s stage object S+1092 " + Hex(c) + " (vtable " + Hex(rd(c)) + ", row 6293): not read");
    }
    case 6:
    case 7: return Make(TopKind::kBusy, s, owner, depth, me + " waits on its message box (S+952 = " + std::to_string(st) + ")");
    case 8:
    case 9: return Make(TopKind::kBusy, s, owner, depth, me + " is changing stage (S+952 = " + std::to_string(st) + ")");
    default: return Make(TopKind::kOther, s, owner, depth, me + " in stage S+952 = " + std::to_string(st) + " (not read)");
  }
}

// The editor's top object from the host H (0 = no host). A message box that is up (1..4) is on top of everything.
template <class R>
Top FindTop(R rd, uint32_t h) {
  const uint32_t box = rd(kBoxPtr), bs = box ? rd(box + kBoxState) : 0;
  if (box && bs >= 1 && bs <= 4)  // CASROW review: the box names itself (cas_pick reads an empty `why` as "nothing moving")
    return Make(TopKind::kBox, box, 0, 0, "the message box " + Hex(box) + " is up (+16 = " + std::to_string(bs) + ", text id " + std::to_string(rd(box + kBoxText)) + ")");
  if (!h) return Make(TopKind::kNone, 0, 0, 0, "the editor's page host is not up");
  const uint32_t p = rd(h + kHPage), a = rd(h + kHOverlayA);
  if (!p) return Make(TopKind::kBusy, 0, h, 0, "no current page (H+948 = 0)");
  if (rd(h + kHOverlayB) || rd(h + kHOverlayPending))
    return Make(TopKind::kOther, rd(h + kHOverlayB), h, 1, "overlay B " + Hex(rd(h + kHOverlayB)) + " / pending " + std::to_string(rd(h + kHOverlayPending)) + ": not read");
  if (!a) {
    if (rd(p) == kListVtable) return Make(TopKind::kList, p, h, 0);
    if (rd(p) == kItemPageVtable) return Make(TopKind::kItemRows, p, h, 0, "item page " + Hex(p) + " showing its own rows (no overlay A)");
    return Make(TopKind::kOther, p, h, 0, "page " + Hex(p) + " (vtable " + Hex(rd(p)) + ")");
  }
  if (rd(a) != kContainerVtable) return Make(TopKind::kOther, a, p, 1, "overlay A " + Hex(a) + " (vtable " + Hex(rd(a)) + ") is not the item page's container");
  if (Closing(rd(a + kState))) return Make(TopKind::kBusy, a, p, 1, "the item page's container " + Hex(a) + " is closing (+16 = " + std::to_string(rd(a + kState)) + ")");
  const uint32_t in = rd(a + kContainerInner);
  if (!in) return Make(TopKind::kBusy, a, p, 1, "the item page's container " + Hex(a) + " has built no panel yet (+936 = 0)");
  if (rd(in) != kPartsPanelVtable)
    return Make(TopKind::kOther, in, a, 2, "the container's panel " + Hex(in) + " (vtable " + Hex(rd(in)) + ": the LETTERING wizard 0x821C0AF8 or the CUSTOM LOGO panel 0x821C1810): not read");
  return PanelTop(rd, in, a, 2);
}
inline const char* TopName(TopKind k) {
  switch (k) {
    case TopKind::kEditPage: return "an edit page (rows by label)";
    case TopKind::kPanel: return "a parts panel (items by number: wwe2k14_cas_pick)";
    case TopKind::kBox: return "the message box";
    case TopKind::kList: return "a list page (wwe2k14_cas_goto walks it)";
    case TopKind::kItemRows: return "an item page's own rows";
    case TopKind::kOther: return "an object of a class not read";
    case TopKind::kBusy: return "nothing settled yet";
    default: return "no editor";
  }
}

// ---- Rows of an edit page ----
// The A handler sub_83407298 takes the row's game label for kinds 1 and 2 only (the element [S+1108] + row of the element table S+656, its
// word +180; kind 1 first tests the row's enable word S+956 + 4 * row, 0 = message 50064). Kind 0 accepts on every row; kind 3 (the side
// page) passes S+360, not the row's label: neither is driven by label.
inline bool KindByLabel(uint32_t kind) { return kind == 1 || kind == 2; }
struct Row {
  uint32_t label = 0;
  bool on = true;
};
// The first row with `label`, or -1.
inline int FindRow(const std::vector<Row>& rows, uint32_t label) {
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].label == label) return int(i);
  return -1;
}
inline std::string RowList(const std::vector<Row>& rows, uint32_t cursor) {
  std::string s;
  for (size_t i = 0; i < rows.size(); ++i)
    s += (i ? ", " : "") + std::to_string(rows[i].label) + (rows[i].on ? "" : " (off)") + (i == cursor ? " <" : "");
  return s.empty() ? "no rows" : s;
}

// ---- The cursor (the shared input mixin's generic move sub_823BC968) ----
// UP (pad table +0) is row - 1, DOWN (+8) row + 1, on the row word mixin+20 of mixin+24 rows; past an end the row wraps when mixin+28 is
// set, else it stays on the end row. LEFT (+12) / RIGHT (+4) move the column mixin+36 the same way (mixin+40 columns, wrap mixin+44).
enum Dir : uint32_t { kDown = 1, kUp = 2 };
inline uint32_t NextRow(uint32_t cur, Dir d, uint32_t n, bool wrap) {
  if (n == 0) return 0;
  if (d == kDown) return cur + 1 < n ? cur + 1 : (wrap ? 0 : n - 1);
  return cur > 0 ? cur - 1 : (wrap ? n - 1 : 0);
}
struct RowPlan {
  Dir dir = kDown;
  uint32_t steps = 0;
};
// From row `cur` to row `want` (0-based) of n: the shorter way round when the rows wrap (down on a tie), else straight.
inline RowPlan PlanRow(uint32_t cur, uint32_t want, uint32_t n, bool wrap) {
  RowPlan p;
  if (n == 0 || want >= n || cur >= n || cur == want) return p;
  if (!wrap) {
    p.dir = want > cur ? kDown : kUp;
    p.steps = want > cur ? want - cur : cur - want;
    return p;
  }
  const uint32_t down = (want + n - cur) % n, up = n - down;
  p.dir = down <= up ? kDown : kUp;
  p.steps = down <= up ? down : up;
  return p;
}

// ---- The message box answer ----
// Kinds 0 / 1: one A (OK). Kinds 2 / 3: the column box+64 (0 YES, 1 NO; it opens on NO), moved by LEFT / RIGHT, then A. The box's A
// (mixin slot 22 -> box vslot 6 sub_8291F7D8) sets +16 = 3; the owner reads the column (sub_82920860: +64 XOR 1 for kinds 2 / 3, so YES = 1).
enum class BoxMove { kPressA, kLeft, kRight, kRefuse };
inline BoxMove PlanBox(uint32_t kind, uint32_t column, Ask answer) {
  if (kind == 0 || kind == 1) return answer == Ask::kOk ? BoxMove::kPressA : BoxMove::kRefuse;
  if (kind == 2 || kind == 3) {
    if (column > 1 || (answer != Ask::kYes && answer != Ask::kNo)) return BoxMove::kRefuse;
    const uint32_t want = answer == Ask::kYes ? 0 : 1;
    if (column == want) return BoxMove::kPressA;
    return want == 0 ? BoxMove::kLeft : BoxMove::kRight;
  }
  return BoxMove::kRefuse;  // 4 / 5 list boxes, 6 / 7 no input: not answered
}

// ---- The route states ----
enum class Status : uint32_t { kIdle = 0, kWorking = 1, kSettled = 2, kStopped = 3, kBox = 4 };
// Why it stopped or refused (cas_row_reason). Routes compare these numbers: never renumber, only append.
enum class Reason : uint32_t {
  kNone = 0,
  kBadArg = 1,      // refused: not a label id nor back / yes / no / ok
  kBusy = 2,        // refused: another driver (cas_goto / cas_pick) holds the editor's presses (src/cas_driver.h)
  kNotServed = 3,   // refused: the top object's class or kind is not driven by this request (the line names it)
  kNoRow = 4,       // refused: no row with that label on the edit page
  kNotReady = 5,    // the top object did not become ready (idle, preview ready) in time after arming
  kStepStuck = 6,   // a cursor press did not land on the next row / column
  kNoChange = 7,    // the A / B / answer changed nothing it is read for, or the page changed under the verb
  kSettleWait = 8,  // the editor did not settle in time after the press
  kHostGone = 9,    // the editor's page host went away
  kBoxRefused = 10, // refused: the answer does not fit the box's kind, or the box's A is off
};
inline constexpr const char* kReasonNames[] = {"none",       "bad_arg",  "busy",      "not_served",  "no_row",     "not_ready",
                                               "step_stuck", "no_change", "settle_wait", "host_gone", "box_refused"};
inline const char* ReasonName(Reason r) {
  const uint32_t i = uint32_t(r);
  return i < sizeof(kReasonNames) / sizeof(kReasonNames[0]) ? kReasonNames[i] : "?";
}

struct Report {
  Status status = Status::kIdle;
  uint32_t request = 0;  // RequestCode of the request taken last
  Reason reason = Reason::kNone;
  TopKind top = TopKind::kNone;  // where it ended
  uint32_t kind = 0;             // an edit page's kind S+948 (1 the part's edit page, 2 the LOGOS page, 3 the side page)
  uint32_t category = 0;         // a parts panel's category P+964 (49 / 50 / 51: the logo lists)
  uint32_t box = 0;              // the message box's text id (status 4)
};
inline bool operator==(const Report& a, const Report& b) {
  return a.status == b.status && a.request == b.request && a.reason == b.reason && a.top == b.top && a.kind == b.kind && a.category == b.category &&
         a.box == b.box;
}
inline Report Armed(uint32_t request) {
  Report r;
  r.status = Status::kWorking;
  r.request = request;
  return r;
}
inline void Settled(Report& r, TopKind top, uint32_t kind, uint32_t category) {
  r.status = Status::kSettled;
  r.reason = Reason::kNone;
  r.top = top, r.kind = kind, r.category = category, r.box = 0;
}
inline void BoxUp(Report& r, uint32_t text_id) {
  r.status = Status::kBox;
  r.reason = Reason::kNone;
  r.top = TopKind::kBox, r.kind = 0, r.category = 0, r.box = text_id;
}
inline void Halt(Report& r, Reason why) {
  r.status = Status::kStopped;
  r.reason = why;
}

enum StateField : uint32_t { kFieldStatus = 1, kFieldLabel, kFieldReason, kFieldTop, kFieldKind, kFieldCategory, kFieldBox, kFieldEnd };
struct StateName {
  const char* name;
  StateField field;
  const char* meaning;
};
inline constexpr StateName kStateNames[] = {
    {"cas_row", kFieldStatus,
     "the CAS row verb (wwe2k14_cas_row): 0 idle, 1 working, 2 settled on the page the game shows next, 3 stopped or refused (cas_row_reason), "
     "4 a message box waits for its answer (cas_row_box)"},
    {"cas_row_label", kFieldLabel, "the request the CAS row verb took last: the row label id, or 1 back, 2 yes, 3 no, 4 ok (stored last)"},
    {"cas_row_reason", kFieldReason, "why the CAS row verb stopped or refused (src/cas_row_core.h Reason; 0 none)"},
    {"cas_row_top", kFieldTop,
     "what the CAS row verb ended on: 1 an edit page, 2 a parts panel, 3 the message box, 4 a list page, 5 an item page's own rows, 6 a class not read"},
    {"cas_row_kind", kFieldKind, "the edit page's kind it ended on (S+948: 1 the part's edit page, 2 the LOGOS page, 3 the side page; 0 otherwise)"},
    {"cas_row_cat", kFieldCategory, "the parts panel's category it ended on (P+964: 49 / 50 / 51 the logo lists; 0 otherwise)"},
    {"cas_row_box", kFieldBox, "the message box's text id while cas_row is 4 (50064 the layer limit; 0 otherwise)"},
};
inline const StateName* FindStateName(const std::string& name) {
  for (const StateName& s : kStateNames)
    if (name == s.name) return &s;
  return nullptr;
}
inline uint32_t FieldValue(const Report& r, uint32_t field) {
  switch (field) {
    case kFieldStatus: return uint32_t(r.status);
    case kFieldLabel: return r.request;
    case kFieldReason: return uint32_t(r.reason);
    case kFieldTop: return uint32_t(r.top);
    case kFieldKind: return r.kind;
    case kFieldCategory: return r.category;
    case kFieldBox: return r.box;
    default: return 0;
  }
}
// A new request: the setting differs from the one taken last ("" = off). The same value twice is no change: set "" between (and wait for
// cas_row 0), as for cas_goto / cas_pick.
inline bool NewValue(const std::string& taken, const std::string& now) { return now != taken; }

}  // namespace wwe2k14::cas_row
