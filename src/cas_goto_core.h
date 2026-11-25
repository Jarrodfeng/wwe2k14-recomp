// S110 CASNAV: the pure part of the Create a Superstar editor go-to (src/cas_goto.h; docs\research\s110\CASNAV.md). Std only, so the unit
// tools/goto/cas_goto_test.cpp drives it on synthetic tables.
//
// The editor's flow table (menu holder slot 1, M+1376: count [data+4], records [data+8]) holds 76-byte big-endian records: +0 id, +4 parent id
// (-1 = a top record), +8 menu, +12 item << 16, +24 label text id, +40 flags, +44 code. The three top records (menu 0, items 0 / 1 / 2) are the
// tabs HEAD / BODY / CLOTHING: sub_833A0DB8 maps tab 0 / 1 / 2 to top item 0 / 1 / 2 (tab 3 is built by sub_833BDDB0, not from the table).
// A list page's rows are the records of the level below its record, minus the rows the game's builder leaves out (sub_833A0628 +88..+150:
// TEMPLATES / 6280 and FACES 6738 on save and edit state; and by the +28 word: 6561 rows only when the superstar's bit sub_822EC988 is 0,
// 6562 rows only when it is 1, e.g. the two TOPS of UPPER BODY), so a row is found by its LABEL on the live page, never by the item index.
// S110 CASARRIVE: also the builder's [root+4788] gate (KeptByPageSet), the way up (PlanUp) and the go-to's progress as route states.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::cas_goto {

struct Rec {
  uint32_t id = 0, parent = 0, menu = 0, item = 0, label = 0, gate = 0, flags = 0, code = 0;
};

constexpr uint32_t kRecordSize = 76;
constexpr uint32_t kNoParent = 0xFFFFFFFFu;
constexpr int kTabs = 4;  // tabs 0..3 (the table's three + the attire tab 3, sub_833BDDB0)
constexpr uint32_t kGateBit0 = 6561, kGateBit1 = 6562;  // sub_833A0628 +10C..+150: the +28 words the builder keeps by the superstar's bit
// Template choosers: code 1003 (sub_833C4308, vtable 0x821BB9B8) and 1008 (sub_833C22F8, vtable 0x821BB5E0) build pages whose kind (vslot 52,
// sub_827BFA00) is 3, and the host's enter of a kind-3 page IS the template change (sub_833BEBF8 +24C..+3A4: the confirm 50056 "Changing the
// Template will reset all settings", or [root+4596] = 1 and the template flow). FACES, SUPERSTAR, DIVA and the gated SUPERSTAR in the table.
constexpr uint32_t kCodeTemplate = 1003, kCodeTemplateGated = 1008;

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }

// `bytes` = count * 76 big-endian bytes, as in guest memory.
inline std::vector<Rec> ParseRecords(const uint8_t* bytes, uint32_t count) {
  std::vector<Rec> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = bytes + size_t(i) * kRecordSize;
    Rec x;
    x.id = Be32(r + 0), x.parent = Be32(r + 4), x.menu = Be32(r + 8), x.item = Be32(r + 12) >> 16, x.label = Be32(r + 24),
    x.gate = Be32(r + 28), x.flags = Be32(r + 40), x.code = Be32(r + 44);
    out.push_back(x);
  }
  return out;
}

inline const Rec* Find(const std::vector<Rec>& recs, uint32_t id) {
  for (const Rec& r : recs)
    if (r.id == id) return &r;
  return nullptr;
}

// Root -> target: path[0] is the top record (its item = the tab), path.back() the target. Returns "" or why not.
inline std::string BuildPath(const std::vector<Rec>& recs, uint32_t target, std::vector<Rec>& path, int& tab) {
  path.clear();
  tab = -1;
  const Rec* r = Find(recs, target);
  if (!r) return "record " + std::to_string(target) + " is not in the editor's table (" + std::to_string(recs.size()) + " records)";
  for (int guard = 0; guard < 64; ++guard) {
    path.insert(path.begin(), *r);
    if (r->parent == kNoParent) {
      if (r->menu != 0) return "record " + std::to_string(r->id) + " has no parent but is in menu " + std::to_string(r->menu) + ", not the top menu 0";
      if (r->item > 2) return "top record " + std::to_string(r->id) + " has item " + std::to_string(r->item) + " (tabs 0..2 come from the table)";
      tab = int(r->item);
      return "";
    }
    const Rec* p = Find(recs, r->parent);
    if (!p) return "record " + std::to_string(r->id) + " names parent " + std::to_string(r->parent) + ", which is not in the table";
    r = p;
  }
  return "the parent chain of record " + std::to_string(target) + " is longer than 64 (a loop?)";
}

// S110 CASARRIVE (the CASSWEEP record 12 stop, re-read here): the builder's first gate, sub_833A0628 +88..+CC, by sub_8338F060(root) = 0 when
// [root+4788] == 512, else 1. Word NOT 512: a record whose +28 word is 6280 is kept only when its label is 6354 (TEMPLATES); word 512: such a
// record is kept only when its label is NOT 6354. What 512 means the code does not say [U] (written by the host ctor sub_833BE878, its update
// sub_833BFA48, sub_833C22F8, sub_833C2670 and the preset page's A sub_833C4448). `word512` = 1 / 0; -1 = not modelled (kept).
constexpr uint32_t kGatePageSet = 6280, kLabelTemplatesRow = 6354;
inline bool KeptByPageSet(const Rec& r, int word512) {
  if (word512 < 0 || r.gate != kGatePageSet) return true;
  return word512 ? r.label != kLabelTemplatesRow : r.label == kLabelTemplatesRow;
}

// Whether the page builder keeps record `r` (sub_833A0628 +10C..+150): `bit` = the superstar's bit (sub_822EC988), -1 when the builder's
// gate is off (sub_822D28A0 == 0: both kept). `word512`: the first gate above (-1 = not modelled).
inline bool Shown(const Rec& r, int bit, int word512 = -1) {
  if (!KeptByPageSet(r, word512)) return false;
  if (bit < 0) return true;
  if (r.gate == kGateBit0) return bit != 1;
  if (r.gate == kGateBit1) return bit != 0;
  return true;
}

// The row of `child` on its parent's live page: `row_labels` = the page's row labels ([[P+976][i]+4], i < [P+972]). Several siblings can
// share a label (UPPER BODY: TOPS twice; LOWER BODY: BOTTOMS twice), so the k-th row with that label is taken, k = the child's rank among
// its same-label siblings by item that the builder keeps (`bit`, Shown: the TOPS twins are 6561 / 6562, one built). -1 = not on the page
// (left out by the builder, or the page is not the parent's).
inline int PickRow(const std::vector<Rec>& recs, const Rec& child, const std::vector<uint32_t>& row_labels, int bit, int word512 = -1) {
  if (!Shown(child, bit, word512)) return -1;
  int rank = 0;
  for (const Rec& s : recs)
    if (s.parent == child.parent && s.menu == child.menu && s.label == child.label && s.item < child.item && Shown(s, bit, word512)) ++rank;
  for (size_t i = 0; i < row_labels.size(); ++i)
    if (row_labels[i] == child.label && rank-- == 0) return int(i);
  return -1;
}

// The game's tab steps from `cur` to `want`: +n = n calls of the next-tab step (sub_833BE790), -n = n of the previous-tab step
// (sub_833BE6A8); straight along 0..3, never round an end: the player's LB on tab 0 is the editor's exit question (sub_833BEBF8 +AE4..+B18),
// and RB on tab 3 (the attire tab) is refused with message 6612 when [..+2628] is set (sub_833BE790 +3C..+68).
inline int TabSteps(int cur, int want) { return want - cur; }

// A path the go-to never drives: one that enters a template chooser (kCodeTemplate / kCodeTemplateGated) below the tab. "" or why.
inline std::string RefusePath(const std::vector<Rec>& path) {
  for (size_t k = 1; k < path.size(); ++k)
    if (path[k].code == kCodeTemplate || path[k].code == kCodeTemplateGated)
      return "record " + std::to_string(path[k].id) + " (label " + std::to_string(path[k].label) + ", code " + std::to_string(path[k].code) +
             ") is a template chooser: entering it is the game's template change (confirm 50056 'reset all settings'), never driven";
  return "";
}

// The id argument: a decimal record id. Returns "" or why not.
inline std::string ParseTarget(const std::string& s, uint32_t& id) {
  if (s.empty() || s.size() > 6) return "give a record id (0..65535) of the editor's flow table";
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return "'" + s + "' is not a record id (digits only)";
    v = v * 10 + uint32_t(c - '0');
  }
  id = v;
  return "";
}

// S110 CASARRIVE: the page classes the way up reads (src/cas_goto.h Step, phase kUp).
constexpr uint32_t kListVtable = 0x821BB3E8;      // a list page (ctor sub_833C16C0); its input mixin (vtable 0x821BB310) at P+28
constexpr uint32_t kItemPageVtable = 0x821BBBA0;  // the item page (code 1002, ctor sub_833C53F0: the list ctor, then its own vtables; run 229's
                                                  // STOPPED line on TOPS); mixin 0x821BBAC8 at P+28, whose B slot 23 is the list's sub_833C14B0
// S110 CASARRIVE fix 1 (run 244: the TOPS page's overlay A was 0x821BE5C0, not the part picker 0x821BEE28 the note had assumed). What an
// item page shows as the host's overlay A (H+952), all [C] from the listings:
//   - The item page's own code asks the host for overlay 7 only (P+992 = 7: its entry sub_833C4A30 +198 / +1BC, its update sub_833C4F38
//     +FC; the rest of its writes are 2 = back and 0), or shows 2-3 rows of its own and no overlay (sub_833C4700, labels 6316 / 6492 /
//     6425). The host's dispatcher case 7 (sub_833BEBF8 +C04..+CB4) builds the CONTAINER sub_833DFE98 (1280 bytes, vtable 0x821BE5C0) as
//     overlay A. No item-page code asks for 6 (the part picker sub_833E9678, 0x821BEE28), 8 or 9: an item page never shows those.
//   - The container's state-1 step (sub_833DFD48) builds ONE inner panel at container+936 by the page's category (container+1140, from the
//     page's P+1108 = the builder's table at 0x821B9940 by record +52; sub_833DFB08): categories 52..57 (LETTERING) a wizard that starts
//     with sub_83401EE8 (vtable 0x821C0AF8), 66 / 67 (CUSTOM LOGO DATA) sub_83409868 (0x821C1810), every other category the PARTS panel
//     sub_833FE938 (0x821C08D8). The pad works the inner panel (its input mixin at +28); the container's own B (sub_8286B6E8 -> the base
//     close) would close it with the inner panel still up.
//   - The parts panel (94 of the table's 112 item pages, TOPS and MUSTACHE among them): its ctor pushes a copy of the superstar on every
//     path (sub_822D3970 at sub_833FE938 +2E4). Its B (mixin 0x821C0800 slot 23, sub_834001F0) closes it (its vslot 7 sub_833FF9C8 puts the
//     part index it opened on, +5172, back, then the base close: +16 = 4, +20 = 0) and restores that copy (sub_822D3A30, +190 / +1AC):
//     the superstar is the one the page was entered with. Its state-4 step (sub_833FAB28) reaches 5 by its vslot 8 (sub_833B3068); the
//     container's update (sub_833DFDC0) then sees +20 = 0 and closes as NOT accepted (sub_823BE7D0, then its vslot 8 sub_833DF7E8: state
//     5), so the host takes the container down with H+948 = P+980 and no grandparent jump (sub_833BFA48 +C0C..+DDC: that needs the
//     overlay's +20): the PARENT is the only correct landing. Its B is a no-op (a sound) while the panel is in a sub-mode (mixin+1000), its
//     enable test sub_833FC6B8 is 0, a pad bit of its mask table 0x8200F510 is held, or the part data is not ready (sub_822DC1A8 of
//     sub_822D2B40): src/cas_goto.h checks those before it presses.
//   - Not driven (refused, reason 16): the LETTERING wizard's B (sub_83400D58) restores the copy only while [root+4536] is set, and the
//     CUSTOM LOGO panel's B (sub_834082F8) puts an index back instead of a copy and is enabled only at its +908 == 6 (sub_83408A40); neither
//     is settled to leave the superstar unchanged.
constexpr uint32_t kContainerVtable = 0x821BE5C0, kContainerInner = 936;
constexpr uint32_t kPartsPanelVtable = 0x821C08D8, kLetteringPanelVtable = 0x821C0AF8, kLogoPanelVtable = 0x821C1810;

// One move on the way up from the page the go-to stands on to its tab's root (a page with no parent).
//   kAtRoot: no parent: go across to the target's tab.
//   kListB: a list page, or an item page showing its own rows (no overlay A): the page's own B (the item page's mixin slot 23 is the
//     list's sub_833C14B0: P+992 = 2; the host's dispatcher makes H+948 = P+980).
//   kPanelB: an item page whose overlay A is the container holding the parts panel: the PANEL's B (above). The host runs no page
//     dispatcher while overlay A is up (sub_833BFA48 +BC8..+BF4), so the item page's own B would only leave 2 in P+992.
//   kNotList / kNoPanel: a page or panel class the go-to does not drive (it stops).
enum class UpMove { kAtRoot, kListB, kPanelB, kNotList, kNoPanel };
inline UpMove PlanUp(uint32_t page_vtable, uint32_t parent, uint32_t overlay_a_vtable, uint32_t inner_vtable) {
  if (!parent) return UpMove::kAtRoot;
  if (page_vtable == kItemPageVtable) {
    if (!overlay_a_vtable) return UpMove::kListB;
    // No panel yet (the container builds it in its state 1, once the model is ready): kPanelB too, src/cas_goto.h waits for it.
    const bool parts = !inner_vtable || inner_vtable == kPartsPanelVtable;
    return overlay_a_vtable == kContainerVtable && parts ? UpMove::kPanelB : UpMove::kNoPanel;
  }
  return page_vtable == kListVtable ? UpMove::kListB : UpMove::kNotList;
}

// S110 SCENESRC: what an ARRIVAL on a page waits for before it is published (src/cas_goto.h ArrivalReady). An item page's container
// (overlay A) counts as settled from its first frame (Busy's item-page rule), before its state-1 step has built the inner panel (+936,
// above) and before the part data is ready: a picture taken at that arrival showed the page half built. So the arrival waits for the
// readiness the way up already waits for before its B (PanelNotReady): cas_goto 2 means "arrived, the panel up" for every route.
//   kNone   no panel on this page: a list page, or an item page showing its own rows (no overlay A): no panel wait
//   kBuild  the container has built no panel yet: wait
//   kParts  the parts panel: wait until PanelNotReady is ""
//   kOther  another overlay class, or the LETTERING / CUSTOM LOGO panel: their readiness was not read; no panel wait (as before)
// Every class then waits for the preview (S110 CASREADY, PreviewAtArrival below) before the arrival is published.
enum class ArrivalPanel { kNone, kBuild, kParts, kOther };
inline ArrivalPanel PanelAtArrival(uint32_t page_vtable, uint32_t overlay_a_vtable, uint32_t inner_vtable) {
  if (page_vtable != kItemPageVtable || !overlay_a_vtable) return ArrivalPanel::kNone;
  if (overlay_a_vtable != kContainerVtable) return ArrivalPanel::kOther;
  if (!inner_vtable) return ArrivalPanel::kBuild;
  return inner_vtable == kPartsPanelVtable ? ArrivalPanel::kParts : ArrivalPanel::kOther;
}

// S110 CASREADY (docs\research\s110\CASREADY.md): after the panel, every arrival waits for the preview (src/cas_goto.h ArrivalReady).
// Run 273: the page and the parts panel were up and the model word ready while the game's round W loading spinner was still drawn (blank
// eyes, no button legend); 120 frames later both were gone. From the listings:
//   - the model ready word sub_822D38A0 (reads only): [[0x836F2DB8]+300]+52 == 0, [[0x836F2DB8]+292]+28 != 0, [[0x836F2DB8]+304]+52 != 1
//     (a rebuild asked, sub_822DE388). The editor's own enable tests and page transitions use it (sub_833FC6B8, sub_833BFA48 +B28).
//   - the game's loading screen: the requester G = [0x83924744] (update sub_827E67C0). The editor's model controller (sub_83391650 +530..
//     +590) asks for screen 0 (G+8 = 1) while the model is not ready and drops it (sub_827E62A8) once it is; the requester shows the
//     screen (G+84 = G+76, sub_827E5938 case 0), closes it (its slot 25, then waits for its slot 24 == 1) and only then clears G+84 and
//     G+4 (sub_827E67C0 +240..+2A8). So the spinner is gone when G+4, G+8 and G+84 are all 0 (no requester: nothing to wait for).
enum class PreviewWait { kReady, kModel, kLoadingScreen };
inline PreviewWait PreviewAtArrival(bool model_ready, uint32_t requester, uint32_t req_state, uint32_t req_pending, uint32_t req_screen) {
  if (!model_ready) return PreviewWait::kModel;
  if (requester && (req_state || req_pending || req_screen)) return PreviewWait::kLoadingScreen;
  return PreviewWait::kReady;
}

// S110 CASARRIVE: the go-to's progress as route states (wait_state / log_state cas_goto ...; src/game_state.h host state sources).
enum class Status : uint32_t { kOff = 0, kRunning = 1, kArrived = 2, kStopped = 3, kRefused = 4 };
// Why it stopped or refused (cas_reason). Routes and the reader compare these numbers: never renumber, only append.
enum class Reason : uint32_t {
  kNone = 0,
  kBadArg = 1,           // refused: the setting is not a record id
  kNotInTable = 2,       // refused: not in the editor's table, or its parent chain is broken (BuildPath)
  kTemplateChooser = 3,  // refused: the path enters a template chooser (RefusePath)
  kTable = 4,            // the editor's flow table is not usable
  kHostGone = 5,         // the editor's page host went away before the go-to ended
  kNotSettled = 6,       // a step (or the first page) did not settle in time
  kWrongPage = 7,        // a step settled on another page than the game's handler sends it to
  kWrongTab = 8,         // a tab step settled on another tab
  kNotList = 9,          // a page the go-to must go through is not a list page
  kTabsOff = 10,         // a tab change is needed and the tabs are off
  kWrongRoot = 11,       // the tab's root page is not the list of the path's top record
  kNoSuperstar = 12,     // the superstar object the page builder reads is not there
  kNoRow = 13,           // the next record is not a row of the live page
  kNoPage = 14,          // a record opens no page but the path goes on below it
  kTemplate = 15,        // the NEW superstar's template pick did not complete
  kNoPicker = 16,        // going up from an item page whose overlay is not the container with the parts panel (name kept: logs compare it)
  kBuilderGate = 17,     // refused: the page builder leaves the record out by [root+4788] (KeptByPageSet)
  kBusy = 18,            // refused: another driver (cas_pick / cas_row) holds the editor's presses (S110 CASROW, src/cas_driver.h)
};
inline constexpr const char* kReasonNames[] = {"none",        "bad_arg",    "not_in_table", "template_chooser", "table",    "host_gone",
                                               "not_settled", "wrong_page", "wrong_tab",    "not_list",         "tabs_off", "wrong_root",
                                               "no_superstar", "no_row",    "no_page",      "template",         "no_picker", "builder_gate",
                                               "busy"};
inline const char* ReasonName(Reason r) {
  const uint32_t i = uint32_t(r);
  return i < sizeof(kReasonNames) / sizeof(kReasonNames[0]) ? kReasonNames[i] : "?";
}

// What the states read. The record is 0 until an arrival (record 0, the HEAD tab, is told by cas_goto 2 with cas_target 0).
struct Report {
  Status status = Status::kOff;
  uint32_t target = 0;  // the record asked for (the setting taken last)
  uint32_t record = 0;  // the record arrived at
  uint32_t level = 0;   // levels entered for this target: the tab's root page is the first
  Reason reason = Reason::kNone;
};
inline bool operator==(const Report& a, const Report& b) {
  return a.status == b.status && a.target == b.target && a.record == b.record && a.level == b.level && a.reason == b.reason;
}
// The transitions (src/cas_goto.h publishes the result; one state line per change).
inline Report Armed(uint32_t target) {
  Report r;
  r.status = Status::kRunning;
  r.target = target;
  return r;
}
inline void Entered(Report& r) { ++r.level; }
inline void Arrive(Report& r) {
  r.status = Status::kArrived;
  r.record = r.target;
  r.reason = Reason::kNone;
}
inline void Halt(Report& r, Reason why) {
  const bool refused = why == Reason::kBadArg || why == Reason::kNotInTable || why == Reason::kTemplateChooser || why == Reason::kBuilderGate ||
                       why == Reason::kBusy;
  r.status = refused ? Status::kRefused : Status::kStopped;
  r.reason = why;
}

// The route shape (S110 CASARRIVE fix 1; run 244's stop sat out a 3000-frame wait_state on cas_record). The script language has no
// either-or wait (src/input_script.h: wait_state compares ONE state), so a route ends its wait on the status and then asserts the record:
//   cvar wwe2k14_cas_goto 36
//   wait_state cas_target 36 60 1     the go-to took it (HOLD 1: a refusal can follow on the next frame); cas_record is 0 from here
//   wait_state cas_goto >=2 3000      arrived (2), stopped (3) or refused (4): a stop ends the wait at once
//   log_state cas_reason              names a stop (0 on an arrival)
//   assert_state cas_record 36        fails now on a stop or refusal
//   screenshot tops                   no settle frames: an item page's arrival is published once its parts panel is up and idle
//                                     (S110 SCENESRC, PanelAtArrival above), and every arrival once the preview model is ready and
//                                     the game's loading screen is down (S110 CASREADY, PreviewAtArrival above)
// (wait_state's default HOLD 2 also covers the publish order: cas_record is stored right after cas_goto, src/cas_goto.h Publish.)
// To revisit the record taken last: `cvar wwe2k14_cas_goto ""`, `wait_state cas_goto 0 60`, then the id (NewTarget below).
// The built-in state names (src/game_state.h AddHostStateSource). Field 0 is never used (GuestState::host_field 0 = not a host state).
enum StateField : uint32_t { kFieldStatus = 1, kFieldRecord, kFieldLevel, kFieldReason, kFieldTarget, kFieldEnd };
struct StateName {
  const char* name;
  StateField field;
  const char* meaning;
};
inline constexpr StateName kStateNames[] = {
    {"cas_goto", kFieldStatus, "the CAS go-to (--wwe2k14_cas_goto): 0 off, 1 running, 2 arrived, 3 stopped, 4 refused"},
    {"cas_record", kFieldRecord, "the record the CAS go-to arrived at (0 until it arrives)"},
    {"cas_level", kFieldLevel, "levels the CAS go-to has entered for its target (the tab's root page is the first)"},
    {"cas_reason", kFieldReason, "why the CAS go-to stopped or refused (src/cas_goto_core.h Reason; 0 none)"},
    {"cas_target", kFieldTarget, "the record the CAS go-to was asked for (the setting it took last)"},
};
inline const StateName* FindStateName(const std::string& name) {
  for (const StateName& s : kStateNames)
    if (name == s.name) return &s;
  return nullptr;
}
inline uint32_t FieldValue(const Report& r, uint32_t field) {
  switch (field) {
    case kFieldStatus: return uint32_t(r.status);
    case kFieldRecord: return r.record;
    case kFieldLevel: return r.level;
    case kFieldReason: return uint32_t(r.reason);
    case kFieldTarget: return r.target;
    default: return 0;
  }
}

// A new target: the setting's value differs from the one taken last ("" = off). The same value set again is no change (the registry's change
// counter does not move either), so a route visits a record again by setting "" first AND waiting for `cas_goto 0` before setting the id:
// consecutive `cvar` lines run in one frame (src/input_script.h OnFrame), so without the wait the go-to only ever sees the id again.
inline bool NewTarget(const std::string& taken, const std::string& now) { return now != taken; }

}  // namespace wwe2k14::cas_goto
