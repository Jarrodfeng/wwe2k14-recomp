// S110 CASROW: host unit for src/cas_row_core.h (the request, the editor's top object on planted objects, the cursor plan, the box answer,
// the route states) and src/cas_driver.h (one owner of the presses). Synthetic values only (no game data). Prints "cas_row_test: ok N
// checks" and returns 0, else the failures and 1. Each rule has a planted twin that must fail it (a wrong reading the rule refuses).
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "cas_driver.h"
#include "cas_goto_core.h"
#include "cas_pick_core.h"
#include "cas_row_core.h"
#include "menu_focus.h"

using namespace wwe2k14::cas_row;
namespace drv = wwe2k14::cas_driver;

static int g_n = 0, g_fail = 0;
static void Check(bool c, const char* what) {
  ++g_n;
  if (!c) {
    ++g_fail;
    std::printf("FAIL: %s\n", what);
  }
}

// A planted guest memory: word address -> value (0 when absent).
struct Mem {
  std::map<uint32_t, uint32_t> w;
  uint32_t operator()(uint32_t a) const {
    auto it = w.find(a);
    return it == w.end() ? 0 : it->second;
  }
  void set(uint32_t a, uint32_t v) { w[a] = v; }
};

constexpr uint32_t H = 0x40000000, ITEM = 0x40001000, CONT = 0x40002000, P = 0x40003000, S = 0x40004000, S2 = 0x40005000, P2 = 0x40006000,
                   BOX = 0x40007000, LIST = 0x40008000;

// The item page with its container, the parts panel on its list.
static Mem ItemPage() {
  Mem m;
  m.set(H + kHPage, ITEM);
  m.set(ITEM, kItemPageVtable);
  m.set(H + kHOverlayA, CONT);
  m.set(CONT, kContainerVtable);
  m.set(CONT + kState, 2);
  m.set(CONT + kContainerInner, P);
  m.set(P, kPartsPanelVtable);
  m.set(P + kState, 2);
  return m;
}
// ... with the part's edit page S up (P+992 = 2) and idle on its rows.
static Mem EditUp() {
  Mem m = ItemPage();
  m.set(P + kPState, 2);
  m.set(P + kPEdit, S);
  m.set(S, kEditPageVtable);
  m.set(S + kState, 2);
  m.set(S + kSKind, 1);
  return m;
}
// ... S showing the LOGOS page S2 (S+952 = 1, S2 at S+1088).
static Mem LogosUp() {
  Mem m = EditUp();
  m.set(S + kSStage, 1);
  m.set(S + kSChild, S2);
  m.set(S2, kEditPageVtable);
  m.set(S2 + kState, 2);
  m.set(S2 + kSKind, 2);
  return m;
}
// ... S2 showing the logo list P2 (S2+952 = 3, P2 at S2+1088; S in its stage 4, S+952 = 4, P in its state 3).
static Mem LogoListUp() {
  Mem m = LogosUp();
  m.set(P + kPState, 3);
  m.set(S + kSStage, 4);
  m.set(S2 + kSStage, 3);
  m.set(S2 + kSChild, P2);
  m.set(P2, kPartsPanelVtable);
  m.set(P2 + kState, 2);
  return m;
}

int main() {
  // ---- The request ----
  Request q;
  Check(ParseRequest("6340", q).empty() && q.ask == Ask::kRow && q.label == 6340 && RequestCode(q) == 6340, "6340: a row label");
  Check(ParseRequest("back", q).empty() && q.ask == Ask::kBack && RequestCode(q) == 1, "back");
  Check(ParseRequest("yes", q).empty() && RequestCode(q) == 2 && ParseRequest("no", q).empty() && RequestCode(q) == 3 && ParseRequest("ok", q).empty() &&
            RequestCode(q) == 4,
        "yes / no / ok: codes 2 / 3 / 4");
  Check(!ParseRequest("", q).empty() && !ParseRequest("63a0", q).empty() && !ParseRequest("5", q).empty() && !ParseRequest("123456", q).empty() &&
            !ParseRequest("Back", q).empty() && q.ask == Ask::kNone,
        "empty, letters, a row number (5, not a label), too long, a capital: refused");

  // ---- The top object ----
  {
    Mem m = ItemPage();
    Top t = FindTop(m, H);
    Check(t.kind == TopKind::kPanel && t.obj == P && t.owner == CONT, "the item page's parts panel on its list: the panel (cas_pick's)");
    m.set(P + kPState, 8);
    Check(FindTop(m, H).kind == TopKind::kPanel, "P+992 = 8 is the list too");
    m.set(CONT + kContainerInner, 0);
    Check(FindTop(m, H).kind == TopKind::kBusy, "the container has built no panel yet: busy");
    Check(FindTop(m, 0).kind == TopKind::kNone, "no host: none");
  }
  {
    Mem m = EditUp();
    Top t = FindTop(m, H);
    Check(t.kind == TopKind::kEditPage && t.obj == S && t.owner == P, "P+992 = 2: the part's edit page S, held by the panel");
    Mem twin = m;  // planted twin: the panel's S slot holds another class
    twin.set(S, 0x821C08D8);
    Check(FindTop(twin, H).kind == TopKind::kOther, "twin: P+996 not the edit page class: a class not read, never driven as S");
    m.set(S + kState, 3);
    Check(FindTop(m, H).kind == TopKind::kBusy, "S closed accepted (+16 = 3): busy (the panel commits next)");
    m.set(S + kState, 2);
    m.set(S + kSStage, 9);
    Check(FindTop(m, H).kind == TopKind::kBusy, "S+952 = 9 (a stage change): busy");
    m.set(S + kSStage, 6);
    Check(FindTop(m, H).kind == TopKind::kBusy, "S+952 = 6 (S waits on its box) and no box up yet: busy");
  }
  {
    Mem m = LogosUp();
    Top t = FindTop(m, H);
    Check(t.kind == TopKind::kEditPage && t.obj == S2 && t.owner == S, "S+952 = 1: the LOGOS page S2 at S+1088 is the top, not S");
    Mem twin = m;  // planted twin: a finder that stops at S would press S's rows under the LOGOS page
    twin.set(S + kSStage, 0);
    Check(FindTop(twin, H).obj == S, "twin: S+952 = 0 means S itself (the walk follows S+952 only)");
    m.set(S + kSChild, 0);
    Check(FindTop(m, H).kind == TopKind::kBusy, "S+952 = 1 with S+1088 = 0: busy (S2 being built)");
  }
  {
    Mem m = LogoListUp();
    Top t = FindTop(m, H);
    Check(t.kind == TopKind::kPanel && t.obj == P2 && t.owner == S2 && t.depth == 5, "S2+952 = 3 with a parts panel at S2+1088: the logo list P2, held by S2");
    m.set(P2, 0x821C1810);
    Check(FindTop(m, H).kind == TopKind::kOther, "a stage object of another class (the CUSTOM LOGO panel 0x821C1810): not read, named");
    m.set(P2, kPartsPanelVtable);
    m.set(P2 + kPState, 4);
    Check(FindTop(m, H).kind == TopKind::kOther, "P2+992 = 4 (the placement tool): not read, named");
  }
  {
    Mem m = LogosUp();
    m.set(kBoxPtr, BOX);
    m.set(BOX + kBoxState, 2);
    Check(FindTop(m, H).kind == TopKind::kBox && FindTop(m, H).obj == BOX, "a box waiting (+16 = 2) is on top of everything");
    // CASROW review: cas_pick's StartStep reads an empty `why` as "nothing moving"; a box with no why let the pick fall through to the
    // panel words of object 0 (a wrong out_of_range refusal). The box names itself.
    Check(!FindTop(m, H).why.empty() && FindTop(m, H).why.find("message box") != std::string::npos, "a box on top names itself (its why is not empty)");
    m.set(BOX + kBoxState, 5);
    Check(FindTop(m, H).obj == S2, "twin: an idle box (+16 = 5) is not on top");
    m.set(BOX + kBoxState, 1);
    Check(FindTop(m, H).kind == TopKind::kBox, "an opening box (+16 = 1) is on top too");
  }
  {
    Mem m;
    m.set(H + kHPage, LIST);
    m.set(LIST, kListVtable);
    Check(FindTop(m, H).kind == TopKind::kList, "no overlay on a list page: the list (cas_goto's)");
    m.set(H + kHOverlayB, 0x40009000);
    Check(FindTop(m, H).kind == TopKind::kOther, "overlay B up: a class not read");
  }

  // ---- Rows ----
  Check(KindByLabel(1) && KindByLabel(2) && !KindByLabel(0) && !KindByLabel(3), "rows by label: kinds 1 and 2 only");
  {
    const std::vector<Row> rows = {{6308, true}, {6340, true}, {6315, true}};
    Check(FindRow(rows, 6340) == 1 && FindRow(rows, 6315) == 2 && FindRow(rows, 6349) == -1, "a row by its label, not its place");
    Check(RowList(rows, 1) == "6308, 6340 <, 6315", "the row list names the cursor");
  }

  // ---- The cursor: the generic move sub_823BC968 ----
  Check(NextRow(0, kDown, 3, true) == 1 && NextRow(2, kDown, 3, true) == 0 && NextRow(0, kUp, 3, true) == 2, "wrap: down past the end is 0, up from 0 the last");
  Check(NextRow(2, kDown, 3, false) == 2 && NextRow(0, kUp, 3, false) == 0, "no wrap: the end row stays");
  {
    RowPlan p = PlanRow(0, 2, 3, true);
    Check(p.dir == kUp && p.steps == 1, "wrap: row 0 -> 2 of 3 is one up");
    p = PlanRow(0, 2, 3, false);
    Check(p.dir == kDown && p.steps == 2, "no wrap: row 0 -> 2 is two down");
    p = PlanRow(1, 1, 3, true);
    Check(p.steps == 0, "on the row: no step");
    p = PlanRow(0, 3, 3, true);
    Check(p.steps == 0, "a row past the end: no plan");
    // a walk through the game's own move reaches every row in the planned presses, both wrap words
    bool ok = true;
    for (bool wrap : {true, false})
      for (uint32_t n : {1u, 3u, 6u})
        for (uint32_t from = 0; from < n; ++from)
          for (uint32_t to = 0; to < n; ++to) {
            uint32_t cur = from, k = 0;
            const uint32_t planned = PlanRow(from, to, n, wrap).steps;
            for (; k < 20; ++k) {
              const RowPlan s = PlanRow(cur, to, n, wrap);
              if (!s.steps) break;
              cur = NextRow(cur, s.dir, n, wrap);
            }
            if (cur != to || k != planned) ok = false;
          }
    Check(ok, "a walk through the generic move lands on the row in the planned presses");
  }

  // ---- The box ----
  Check(PlanBox(0, 0, Ask::kOk) == BoxMove::kPressA && PlanBox(1, 0, Ask::kOk) == BoxMove::kPressA, "OK boxes: ok is one A");
  Check(PlanBox(0, 0, Ask::kYes) == BoxMove::kRefuse, "twin: yes on an OK box is refused (no guess)");
  Check(PlanBox(2, 1, Ask::kYes) == BoxMove::kLeft && PlanBox(2, 0, Ask::kYes) == BoxMove::kPressA, "YES / NO opening on NO: LEFT, then A");
  Check(PlanBox(3, 0, Ask::kNo) == BoxMove::kRight && PlanBox(3, 1, Ask::kNo) == BoxMove::kPressA, "no: RIGHT to column 1, then A");
  Check(PlanBox(2, 1, Ask::kOk) == BoxMove::kRefuse && PlanBox(2, 2, Ask::kYes) == BoxMove::kRefuse && PlanBox(4, 0, Ask::kOk) == BoxMove::kRefuse &&
            PlanBox(6, 0, Ask::kOk) == BoxMove::kRefuse,
        "ok on a YES / NO box, a column past 1, a list box, a no-input box: refused");

  // ---- One owner of the presses ----
  drv::g_owner = drv::kNone;
  Check(drv::Claim(drv::kGoto) && drv::g_owner == drv::kGoto, "the go-to takes the editor");
  Check(!drv::Claim(drv::kPick) && !drv::Claim(drv::kRow) && drv::g_owner == drv::kGoto, "the pick and the row verb are refused while it drives");
  Check(drv::Claim(drv::kGoto), "the same driver may re-arm");
  drv::Release(drv::kPick);
  Check(drv::g_owner == drv::kGoto, "twin: another driver's release frees nothing");
  drv::Release(drv::kGoto);
  Check(drv::g_owner == drv::kNone && drv::Claim(drv::kRow) && drv::g_owner == drv::kRow, "released: the next one takes it");
  drv::Release(drv::kRow);

  // ---- The route states ----
  Report r = Armed(6340);
  Check(FieldValue(r, kFieldStatus) == 1 && FieldValue(r, kFieldLabel) == 6340, "armed: working, request 6340");
  Settled(r, TopKind::kEditPage, 2, 0);
  Check(FieldValue(r, kFieldStatus) == 2 && FieldValue(r, kFieldTop) == 1 && FieldValue(r, kFieldKind) == 2 && FieldValue(r, kFieldBox) == 0,
        "settled on the LOGOS page: 2, top 1, kind 2");
  BoxUp(r, 50064);
  Check(FieldValue(r, kFieldStatus) == 4 && FieldValue(r, kFieldBox) == 50064 && FieldValue(r, kFieldTop) == 3, "a box: 4, its id");
  Halt(r, Reason::kNoRow);
  Check(FieldValue(r, kFieldStatus) == 3 && FieldValue(r, kFieldReason) == 4, "stopped: 3, reason 4");
  Check(std::string(ReasonName(Reason::kBoxRefused)) == "box_refused" && std::string(ReasonName(Reason(99))) == "?" &&
            sizeof(kReasonNames) / sizeof(kReasonNames[0]) == uint32_t(Reason::kBoxRefused) + 1,
        "a name for every reason");
  Check(NewValue("6340", "6678") && !NewValue("back", "back") && NewValue("back", ""), "a different value is a new request");
  bool clash = false, fields_ok = true;
  for (const StateName& s : kStateNames) {
    if (s.field == 0 || s.field >= kFieldEnd) fields_ok = false;
    for (const wwe2k14::cas_goto::StateName& g : wwe2k14::cas_goto::kStateNames)
      if (std::string(s.name) == g.name) clash = true;
    for (const wwe2k14::cas_pick::StateName& p : wwe2k14::cas_pick::kStateNames)
      if (std::string(s.name) == p.name) clash = true;
    for (const wwe2k14::menu_focus::HostName& h : wwe2k14::menu_focus::kHostNames)
      if (std::string(s.name) == h.name) clash = true;
  }
  Check(!clash && fields_ok && sizeof(kStateNames) / sizeof(kStateNames[0]) == kFieldEnd - 1 && FindStateName("cas_row_box"),
        "cas_row_* beside cas_goto / cas_pick / menu_*: no clash");

  if (g_fail) {
    std::printf("cas_row_test: %d of %d checks FAILED\n", g_fail, g_n);
    return 1;
  }
  std::printf("cas_row_test: ok %d checks\n", g_n);
  return 0;
}
