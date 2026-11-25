// S110 CASNAV: host unit for src/cas_goto_core.h (the record parse, the root -> record path, the row pick by label, the tab steps, the
// argument parse). Synthetic tables only (no game data). Prints "cas_goto_test: ok N checks" and returns 0, else the failures and 1.
#include <cstdio>
#include <string>
#include <vector>

#include "cas_goto_core.h"
#include "menu_focus.h"  // S110 CASARRIVE: the cas_* names must not meet the menu_* ones

using namespace wwe2k14::cas_goto;

static int g_n = 0, g_fail = 0;
static void Check(bool c, const char* what) {
  ++g_n;
  if (!c) {
    ++g_fail;
    std::printf("FAIL: %s\n", what);
  }
}

static Rec R(uint32_t id, uint32_t parent, uint32_t menu, uint32_t item, uint32_t label, uint32_t code, uint32_t gate = 0) {
  Rec r;
  r.id = id, r.parent = parent, r.menu = menu, r.item = item, r.label = label, r.code = code, r.gate = gate;
  return r;
}

static void Put(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  b[off] = uint8_t(v >> 24), b[off + 1] = uint8_t(v >> 16), b[off + 2] = uint8_t(v >> 8), b[off + 3] = uint8_t(v);
}

int main() {
  // A small tree in the editor's shape: three top records (tabs), a list under tab 2 with two same-label leaves and a nested list.
  std::vector<Rec> t = {
      R(0, kNoParent, 0, 0, 100, 1001), R(1, kNoParent, 0, 1, 101, 1001), R(2, kNoParent, 0, 2, 102, 1001),
      R(3, 0, 1, 0, 200, 1003),         R(4, 0, 1, 1, 201, 1001),          R(5, 4, 4, 0, 300, 1003),
      R(6, 2, 3, 0, 400, 1007),         R(7, 2, 3, 1, 401, 1001),          R(8, 7, 5, 0, 500, 1002),
      R(9, 7, 5, 1, 501, 1002),         R(10, 7, 5, 2, 501, 1002),         R(11, 7, 5, 3, 502, 1001),
      R(12, 11, 6, 0, 600, 1002),
  };

  // ParseRecords: big-endian words at +0 / +4 / +8 / +12 (item << 16) / +24 / +40 / +44.
  std::vector<uint8_t> raw(2 * kRecordSize, 0);
  Put(raw, 0, 7), Put(raw, 4, 2), Put(raw, 8, 3), Put(raw, 12, (1u << 16) | 0x0055), Put(raw, 24, 401), Put(raw, 28, 6562), Put(raw, 40, 0x12),
      Put(raw, 44, 1001);
  Put(raw, 76 + 0, 2), Put(raw, 76 + 4, kNoParent), Put(raw, 76 + 12, 2u << 16), Put(raw, 76 + 24, 102);
  const std::vector<Rec> p = ParseRecords(raw.data(), 2);
  Check(p.size() == 2, "parse: two records");
  Check(p[0].id == 7 && p[0].parent == 2 && p[0].menu == 3 && p[0].item == 1 && p[0].label == 401 && p[0].gate == 6562 && p[0].flags == 0x12 &&
            p[0].code == 1001,
        "parse: every word of record 0 (item = the high half of +12, gate = +28)");
  Check(p[1].parent == kNoParent && p[1].item == 2 && p[1].label == 102, "parse: a top record");

  // BuildPath: root first, tab from the top record's item.
  std::vector<Rec> path;
  int tab = -9;
  Check(BuildPath(t, 12, path, tab).empty(), "path to 12 builds");
  Check(tab == 2 && path.size() == 4 && path[0].id == 2 && path[1].id == 7 && path[2].id == 11 && path[3].id == 12, "path 2 > 7 > 11 > 12, tab 2");
  Check(BuildPath(t, 1, path, tab).empty() && tab == 1 && path.size() == 1 && path[0].id == 1, "a top record is its own path (tab 1)");
  Check(BuildPath(t, 5, path, tab).empty() && tab == 0 && path.size() == 3 && path[1].id == 4, "path 0 > 4 > 5, tab 0");
  Check(!BuildPath(t, 99, path, tab).empty() && tab == -1, "an unknown record is refused");
  std::vector<Rec> orphan = t;
  orphan.push_back(R(20, 77, 9, 0, 1, 1002));
  Check(!BuildPath(orphan, 20, path, tab).empty(), "a missing parent is refused");
  std::vector<Rec> loop = {R(30, 31, 1, 0, 1, 1001), R(31, 30, 1, 1, 2, 1001)};
  Check(!BuildPath(loop, 30, path, tab).empty(), "a parent loop is refused");
  std::vector<Rec> badtop = {R(40, kNoParent, 2, 0, 1, 1001)};
  Check(!BuildPath(badtop, 40, path, tab).empty(), "a parentless record outside menu 0 is refused");

  // PickRow: by label on the live rows; the k-th of same-label siblings; gated rows shift the index. Bit -1 = the builder's +28 gate is off.
  const Rec* r9 = Find(t, 9);
  const Rec* r10 = Find(t, 10);
  const Rec* r11 = Find(t, 11);
  Check(PickRow(t, *r9, {500, 501, 501, 502}, -1) == 1, "first of two same-label rows");
  Check(PickRow(t, *r10, {500, 501, 501, 502}, -1) == 2, "second of two same-label rows");
  Check(PickRow(t, *r11, {500, 501, 501, 502}, -1) == 3, "a unique label");
  Check(PickRow(t, *r11, {501, 501, 502}, -1) == 2, "a gated row before it shifts the row");
  Check(PickRow(t, *r10, {500, 501, 502}, -1) == -1, "the second same-label row missing: not found");
  Check(PickRow(t, *r11, {}, -1) == -1, "an empty page: not found");

  // S110 CASNAV review: same-label twins split by the +28 gate (UPPER BODY: TOPS 6561 / TOPS 6562). sub_833A0628 +10C..+150 builds only the
  // one the superstar's bit (sub_822EC988) allows, so the k-th-label rank must count only the siblings the builder keeps.
  std::vector<Rec> g = {R(50, 2, 3, 0, 700, 1002), R(51, 2, 3, 1, 701, 1002, 6561), R(52, 2, 3, 2, 701, 1002, 6562), R(53, 2, 3, 3, 702, 1002)};
  const Rec& g51 = g[1];
  const Rec& g52 = g[2];
  Check(PickRow(g, g51, {700, 701, 702}, 0) == 1, "bit 0: the 6561 twin is the one row with its label");
  Check(PickRow(g, g52, {700, 701, 702}, 0) == -1, "bit 0: the 6562 twin is not built: not found");
  Check(PickRow(g, g52, {700, 701, 702}, 1) == 1, "bit 1: the 6562 twin is the one row (its 6561 sibling is not counted)");
  Check(PickRow(g, g51, {700, 701, 702}, 1) == -1, "bit 1: the 6561 twin is not built: never its sibling's row");
  Check(PickRow(g, g52, {700, 701, 701, 702}, -1) == 2, "gate off: both built, the second by item");
  Check(Shown(g51, 0) && !Shown(g51, 1) && Shown(g52, 1) && !Shown(g52, 0) && Shown(g51, -1) && Shown(g[0], 1), "Shown follows +10C..+150");

  // TabSteps: straight along 0..3, never round the ends (the game's LB on tab 0 is the exit question, sub_833BEBF8 +AE4; tab 3 is the
  // attire tab whose RB the game can refuse, sub_833BE790 +54).
  Check(TabSteps(0, 2) == 2, "0 -> 2: two forward");
  Check(TabSteps(2, 0) == -2, "2 -> 0: two back, not round through tab 3");
  Check(TabSteps(1, 1) == 0, "same tab: none");
  Check(TabSteps(3, 0) == -3, "3 -> 0: three back, not the wrap");
  Check(TabSteps(1, 0) == -1, "1 -> 0: one back");
  Check(TabSteps(0, 1) == 1, "0 -> 1: one forward");

  // RefusePath: a template chooser (code 1003 / 1008) anywhere below the tab is refused; other pages are not.
  std::vector<Rec> tp;
  int ttab = -1;
  std::vector<Rec> tt = {R(0, kNoParent, 0, 0, 100, 1001), R(4, 0, 1, 1, 6354, 1001), R(23, 4, 4, 0, 6593, 1003), R(25, 4, 4, 2, 6280, 1008),
                         R(3, 0, 1, 0, 6738, 1003), R(10, 0, 1, 7, 900, 1002)};
  Check(BuildPath(tt, 23, tp, ttab).empty() && !RefusePath(tp).empty(), "SUPERSTAR template (1003) refused");
  Check(BuildPath(tt, 25, tp, ttab).empty() && !RefusePath(tp).empty(), "the gated SUPERSTAR template (1008) refused");
  Check(BuildPath(tt, 3, tp, ttab).empty() && !RefusePath(tp).empty(), "FACES (1003 under the tab root) refused");
  Check(BuildPath(tt, 4, tp, ttab).empty() && RefusePath(tp).empty(), "the TEMPLATES list itself is a plain list");
  Check(BuildPath(tt, 10, tp, ttab).empty() && RefusePath(tp).empty(), "an item page (1002) is not refused");

  uint32_t id = 0;
  Check(ParseTarget("76", id).empty() && id == 76, "parse 76");
  Check(!ParseTarget("", id).empty() && !ParseTarget("7a", id).empty() && !ParseTarget("1234567", id).empty(), "bad ids refused");

  // S110 CASARRIVE: the builder's [root+4788] gate (sub_833A0628 +88..+CC): +28 word 6280 kept only with label 6354 while the word is not 512,
  // only without it while the word is 512 (CASSWEEP record 12: BODY > TEMPLATES, label 6354, 6280, word 512 -> left out).
  const Rec rec12 = R(12, 1, 2, 0, 6354, 1009, 6280);
  const Rec other6280 = R(13, 1, 2, 1, 6593, 1008, 6280);
  Check(!KeptByPageSet(rec12, 1) && KeptByPageSet(rec12, 0) && KeptByPageSet(rec12, -1), "6354 / 6280: out only with the word 512");
  Check(KeptByPageSet(other6280, 1) && !KeptByPageSet(other6280, 0), "another label / 6280: out only without the word 512");
  Check(KeptByPageSet(R(14, 1, 2, 2, 6354, 1001), 1), "6354 without the 6280 word: kept");
  std::vector<Rec> bt = {R(1, kNoParent, 0, 1, 101, 1001), rec12, R(15, 1, 2, 1, 777, 1002), R(16, 1, 2, 2, 777, 1002, 6280)};
  Check(PickRow(bt, bt[1], {777, 777}, -1, 1) == -1, "word 512: record 12 is no row");
  Check(PickRow(bt, bt[2], {777}, -1, 0) == 0 && PickRow(bt, bt[3], {777}, -1, 0) == -1, "word not 512: the 6280 twin is not built");
  Check(PickRow(bt, bt[3], {777, 777}, -1, 1) == 1, "word 512: both 777 rows built, the 6280 one second");

  // S110 CASARRIVE: the route states. One target: armed, levels entered, arrived.
  Report r = Armed(12);
  Check(r.status == Status::kRunning && r.target == 12 && r.record == 0 && r.level == 0, "armed: running, no record yet");
  for (int k = 0; k < 4; ++k) Entered(r);
  Arrive(r);
  Check(FieldValue(r, kFieldStatus) == 2 && FieldValue(r, kFieldRecord) == 12 && FieldValue(r, kFieldLevel) == 4 && FieldValue(r, kFieldReason) == 0 &&
            FieldValue(r, kFieldTarget) == 12,
        "arrived at 12 after 4 levels (2 > 7 > 11 > 12)");
  // Refused and stopped.
  Report bad = Armed(99);
  Check(!BuildPath(t, 99, path, tab).empty(), "99 is not in the table");
  Halt(bad, Reason::kNotInTable);
  Check(bad.status == Status::kRefused && FieldValue(bad, kFieldReason) == 2 && bad.record == 0, "not in the table: refused (4), reason 2");
  Report tpl = Armed(23);
  Halt(tpl, Reason::kTemplateChooser);
  Report gate = Armed(12);
  Halt(gate, Reason::kBuilderGate);
  Report arg;
  Halt(arg, Reason::kBadArg);
  Check(tpl.status == Status::kRefused && gate.status == Status::kRefused && arg.status == Status::kRefused, "template / gate / argument: refused");
  Report stop = Armed(5);
  Entered(stop);
  Halt(stop, Reason::kNoRow);
  Check(FieldValue(stop, kFieldStatus) == 3 && FieldValue(stop, kFieldReason) == 13 && stop.level == 1 && stop.record == 0, "a stop: 3, reason 13");
  Check(std::string(ReasonName(Reason::kNoPicker)) == "no_picker" && std::string(ReasonName(Reason::kBuilderGate)) == "builder_gate" &&
            std::string(ReasonName(Reason(99))) == "?",
        "reason names");

  // A second target after an arrival: the setting changes, the report starts again.
  Check(NewTarget("8", "5") && !NewTarget("8", "8") && NewTarget("8", "") && NewTarget("", "8"), "a different value is a new target");
  Report first = Armed(8);
  Entered(first), Entered(first), Entered(first);
  Arrive(first);
  Report second = Armed(5);
  Check(first.record == 8 && second.status == Status::kRunning && second.record == 0 && second.level == 0 && second.target == 5 && !(first == second),
        "re-armed: running, record and level back to 0");
  // The way up from record 8 (an item page at depth 2 under 2 > 7, tab 2) to record 5 (tab 0, 0 > 4 > 5): the parts panel's B, list B, root,
  // two tabs back. S110 CASARRIVE fix 1 (run 244): an item page's overlay A is the container 0x821BE5C0; the B goes to the panel inside it.
  struct Page {
    uint32_t vtable, parent;
  };
  const Page pages[] = {{kListVtable, 0}, {kListVtable, 1}, {kItemPageVtable, 2}};  // [1] root of tab 2, [2] record 7's list, [3] record 8
  uint32_t at = 3;
  std::vector<UpMove> moves;
  for (int guard = 0; guard < 8; ++guard) {
    const Page& pg = pages[at - 1];
    const bool item = pg.vtable == kItemPageVtable;
    const UpMove m = PlanUp(pg.vtable, pg.parent, item ? kContainerVtable : 0, item ? kPartsPanelVtable : 0);
    moves.push_back(m);
    if (m != UpMove::kPanelB && m != UpMove::kListB) break;
    at = pg.parent;
  }
  Check(moves.size() == 3 && moves[0] == UpMove::kPanelB && moves[1] == UpMove::kListB && moves[2] == UpMove::kAtRoot && at == 1,
        "up from depth 2: the parts panel's B, the list's B, then the root");
  Check(BuildPath(t, 8, path, tab).empty() && tab == 2 && path.size() == 3, "record 8: tab 2, depth 2");
  Check(BuildPath(t, 5, path, tab).empty() && tab == 0 && TabSteps(2, tab) == -2, "record 5: tab 0, two previous-tab steps from tab 2");
  // The class choice on an item page (run 244's overlay was the container; the CASARRIVE note's picker 0x821BEE28 is never an item page's).
  Check(PlanUp(kItemPageVtable, 0x1000, kContainerVtable, kPartsPanelVtable) == UpMove::kPanelB, "container + parts panel: the panel's B");
  Check(PlanUp(kItemPageVtable, 0x1000, kContainerVtable, kLetteringPanelVtable) == UpMove::kNoPanel &&
            PlanUp(kItemPageVtable, 0x1000, kContainerVtable, kLogoPanelVtable) == UpMove::kNoPanel,
        "container + the LETTERING wizard or the CUSTOM LOGO panel: refused (their B is not settled to restore)");
  Check(PlanUp(kItemPageVtable, 0x1000, kContainerVtable, 0) == UpMove::kPanelB,
        "container with no panel yet: the panel's way (the go-to waits for the panel), never the container's own B");
  Check(PlanUp(kItemPageVtable, 0x1000, 0x821BEE28, 0) == UpMove::kNoPanel && PlanUp(kItemPageVtable, 0x1000, 0x821BF0C8, 0) == UpMove::kNoPanel,
        "another overlay class on an item page (the part picker, 0x821BF0C8): refused");
  Check(PlanUp(kItemPageVtable, 0x1000, 0, 0) == UpMove::kListB, "an item page showing its own rows (no overlay): its own B, as a list's");
  Check(PlanUp(0x821BB9B8, 0x1000, 0, 0) == UpMove::kNotList && PlanUp(kItemPageVtable, 0, kContainerVtable, kPartsPanelVtable) == UpMove::kAtRoot,
        "other pages; a root");

  // S110 SCENESRC: what an arrival waits for. A list page and an item page showing its own rows: nothing; the container before its panel
  // is built: the build; the parts panel: its readiness (PanelNotReady); another panel class or overlay: published as built.
  Check(PanelAtArrival(kListVtable, 0, 0) == ArrivalPanel::kNone && PanelAtArrival(kListVtable, kContainerVtable, kPartsPanelVtable) == ArrivalPanel::kNone,
        "arrival on a list page: no panel wait (even with an overlay up)");
  Check(PanelAtArrival(kItemPageVtable, 0, 0) == ArrivalPanel::kNone, "arrival on an item page showing its own rows: no panel wait");
  Check(PanelAtArrival(kItemPageVtable, kContainerVtable, 0) == ArrivalPanel::kBuild, "the container before its state-1 step: wait for the panel's build");
  Check(PanelAtArrival(kItemPageVtable, kContainerVtable, kPartsPanelVtable) == ArrivalPanel::kParts, "the parts panel: wait for its readiness");
  Check(PanelAtArrival(kItemPageVtable, kContainerVtable, kLetteringPanelVtable) == ArrivalPanel::kOther &&
            PanelAtArrival(kItemPageVtable, kContainerVtable, kLogoPanelVtable) == ArrivalPanel::kOther &&
            PanelAtArrival(kItemPageVtable, 0x821BEE28, 0) == ArrivalPanel::kOther,
        "LETTERING / CUSTOM LOGO / another overlay class: published as built");

  // S110 CASREADY: the preview at an arrival. The model word first; then the game's screen requester G: a request pending (+8), a screen
  // shown or closing (+84, state +4) all wait; G idle, or no G at all, is ready. Run 273's arrival: model ready, the spinner screen
  // still up (state 2, closing) -> wait.
  Check(PreviewAtArrival(false, 0x40001000, 0, 0, 0) == PreviewWait::kModel && PreviewAtArrival(false, 0, 0, 0, 0) == PreviewWait::kModel,
        "the model not ready: wait for the model, whatever the requester says");
  Check(PreviewAtArrival(true, 0x40001000, 0, 1, 0) == PreviewWait::kLoadingScreen, "a loading screen asked for (G+8 = 1): wait");
  Check(PreviewAtArrival(true, 0x40001000, 1, 0, 0x40002000) == PreviewWait::kLoadingScreen, "the spinner shown (G+84 = G+76, state 1): wait");
  Check(PreviewAtArrival(true, 0x40001000, 2, 0, 0x40002000) == PreviewWait::kLoadingScreen, "run 273: the spinner closing (state 2): wait");
  Check(PreviewAtArrival(true, 0x40001000, 0, 0, 0x40002000) == PreviewWait::kLoadingScreen, "a screen still held with state 0: wait");
  // CASREADY review: the state word alone (each other term has its own case above). The close clears G+84 (sub_827E67C0 +28C) before
  // G+4 (+2A8): state 3 with no screen is that window, seen by a reader on another thread; dropping `req_state` fails here.
  Check(PreviewAtArrival(true, 0x40001000, 3, 0, 0) == PreviewWait::kLoadingScreen, "the close's last step (state 3, G+84 already 0): wait");
  Check(PreviewAtArrival(true, 0x40001000, 0, 0, 0) == PreviewWait::kReady, "the requester idle: ready");
  Check(PreviewAtArrival(true, 0, 3, 1, 0x40002000) == PreviewWait::kReady, "no requester object: nothing to wait for");

  // The names: found, fields 1..5, none of them a menu_* name (a second host-state source beside the menu one).
  Check(FindStateName("cas_goto") && FindStateName("cas_goto")->field == kFieldStatus && FindStateName("cas_target") && !FindStateName("cas_page"),
        "names found");
  bool clash = false, fields_ok = true;
  for (const StateName& s : kStateNames) {
    if (s.field == 0 || s.field >= kFieldEnd) fields_ok = false;
    for (const wwe2k14::menu_focus::HostName& h : wwe2k14::menu_focus::kHostNames)
      if (std::string(s.name) == h.name) clash = true;
  }
  Check(!clash && fields_ok && sizeof(kStateNames) / sizeof(kStateNames[0]) == kFieldEnd - 1, "cas_* beside menu_*: no clash, fields 1..5");

  if (g_fail) {
    std::printf("cas_goto_test: %d of %d checks FAILED\n", g_fail, g_n);
    return 1;
  }
  std::printf("cas_goto_test: ok %d checks\n", g_n);
  return 0;
}
