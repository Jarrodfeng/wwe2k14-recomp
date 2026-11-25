// S110 CASPICK: the pure part of the Create a Superstar item pick (src/cas_pick.h; docs\research\s110\CASPICK.md). Std only, so the unit
// tools/goto/cas_pick_test.cpp drives it.
#pragma once

#include <cstdint>
#include <string>

namespace wwe2k14::cas_pick {

// The setting: the item, 1-based as the parts panel shows it ("5" = the item the panel shows as 5/51). "" = off (src/cas_pick.h Arm).
inline std::string ParseItem(const std::string& s, uint32_t& k) {
  if (s.empty() || s.size() > 4) return "give an item number 1..9999, 1-based as the parts panel shows it (5 = \"5/51\")";
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return "'" + s + "' is not an item number (digits only)";
    v = v * 10 + uint32_t(c - '0');
  }
  if (v == 0) return "item numbers start at 1, as the panel shows them";
  k = v;
  return "";
}

// A step of the panel's cursor, by what it does to the highlighted index (sub_833F8B08, 0-based: L's vslot 2 = [L+84], plus P+5184, brought
// into 0..[L+20]-1). The values are the panel's step codes P+1032: the pad's down bit starts sub_833FC168(P, 1), whose commit sub_833FE7E8
// calls the list's vslot 5 sub_83431080 (+84..+A0: [L+84] += 1, back to 0 when it reaches [L+20]); the up bit starts (P, 2) -> vslot 6
// sub_83431138 (+248..+260: [L+84] -= 1, [L+20] - 1 below 0). So kDown = the next item (+1), kUp = the previous one (-1), both wrapping.
// The list is built by sub_833FADA0 +148..+178 (base ctor sub_83431A18, vtable 0x821C0774, stored at P+948). CASPICK review: read, not guessed.
enum Dir : uint32_t { kDown = 1, kUp = 2 };
inline uint32_t NextIndex(uint32_t cur, Dir d, uint32_t n) {
  if (n == 0) return 0;
  return d == kDown ? (cur + 1) % n : (cur + n - 1) % n;
}

// From index `cur` to item k (1-based) on a list of n items: the shorter way round, down on a tie. steps = 0 when k is not 1..n.
struct Plan {
  Dir dir = kDown;
  uint32_t steps = 0;
};
inline Plan PlanSteps(uint32_t cur, uint32_t k, uint32_t n) {
  Plan p;
  if (n == 0 || k == 0 || k > n) return p;
  const uint32_t want = k - 1, c = cur % n;
  const uint32_t down = (want + n - c) % n, up = (n - down) % n;
  p.dir = down <= up ? kDown : kUp;
  p.steps = down <= up ? down : up;
  return p;
}

// A committed step landed where it was asked to. It can fail on the game's own terms only: the list's move returns without moving while its
// last scroll still runs ([L+80] and [L+16] set) or while [[L+112]+112] == 0 (sub_83431080 +0..+24); a player's press is lost the same way.
inline bool StepLanded(uint32_t prev, uint32_t now, Dir asked, uint32_t n) { return now == NextIndex(prev, asked, n); }

// The A handler sub_833FFE20 by the panel's mode P+980 (its jump table at +230, the 10 targets read from the listing): 0 / 2 / 6 / 8 / 9 ->
// vslot 66 sub_833F8428 (P+992 = 2, the accepted out transition): a plain accept, driven. The rest is not a plain accept and is refused:
// 1 -> vslot 6 sub_833FF868 and the list's own commit; 3 -> the layered path (P+992 = 4 or 5, vslot +24); 4 / 7 -> a sound only (nothing
// accepted); 5 -> vslot 67 sub_833F84D0 (not read). The container's description sets the mode (sub_833DFB08 +180 / +190: 3 for the
// categories 49..57 and 72, 6 for category 4).
inline bool PlainAccept(uint32_t mode) { return mode == 0 || mode == 2 || mode == 6 || mode == 8 || mode == 9; }
inline const char* ModeName(uint32_t mode) {
  switch (mode) {
    case 1: return "the panel's commit (vslot 6 sub_833FF868) and the list's own step";
    case 3: return "the layered path (P+992 = 4 or 5: the next editing stage)";
    case 4:
    case 7: return "a sound only (nothing is accepted)";
    case 5: return "vslot 67 sub_833F84D0 (not read)";
    default: return PlainAccept(mode) ? "the accept (vslot 66 sub_833F8428)" : "outside the jump table (0..9)";
  }
}

// S110 CASROW: the logo list P2 (LOGOSTAGE.md claims 4 / 11 / 12): a parts panel built by the LOGOS page's rows "? DESIGNS" / DESIGNS / FLAGS
// (categories 49 / 50 / 51, mode 3). Mode 3's A goes to the placement tool (P+992 = 4) unless P+972 == 10, where it calls vslot 6 at once
// (sub_833FFE20 +2F4..+334, no list reset); vslot 6 sub_833FF868 keeps the logo with the default placement (its 49..57 branch). So a NESTED
// mode-3 panel of those categories is committed as the game's own P+972 == 10 path does; at the item page level mode 3 stays refused (that
// page is not read), and edit mode never drives mode 3 (its A opens the placement tool, not read).
inline bool NestedLogoCommit(bool nested, uint32_t mode, uint32_t category, bool edit) {
  return nested && !edit && mode == 3 && (category == 49 || category == 50 || category == 51);
}
// S110 CASROW: wwe2k14_cas_pick_mode: "" or "commit" = the panel's commit (fix 3); "edit" = the player's accepting A instead, ending with the
// part's edit page S up and idle (P+992 = 2), so a route takes S's rows next (wwe2k14_cas_row). Returns "" or why not.
inline std::string ParseMode(const std::string& s, bool& edit) {
  edit = s == "edit";
  if (s.empty() || s == "commit" || s == "edit") return "";
  return "'" + s + "' is not a pick mode (commit, the default, or edit)";
}

// CASPICK fix 3 (rule 6, after runs 281 and 294): the keep is the parts panel's own commit, not a walk of the pages after the A. Both of the
// game's accept paths end in the panel's vslot 6 sub_833FF868: mode 1's A calls it at once (sub_833FFE20 jump target 8340007C: the this-28
// thunk sub_8291F7B0 -> vslot 6, then sub_834317B8 on the list), and in modes 0 / 2 / 6 / 8 / 9 the A only starts the edit stage (P+992 = 2,
// sub_833F8428; the state switch sub_833FA688 +190: 2 -> slot 50 sub_833FFAD8), which builds the part's edit page S and calls vslot 6 when S
// closes accepted (+11C). S and its stages (S's own modes S+952, the panel's state 3 = slot 51 sub_833FCCC8 waiting on them: run 294's colour
// picker) write nothing to the superstar until one of their edit rows is used (S's ctor sub_834070A0 / sub_83406840 / sub_834068E0 and its
// update sub_83407F98 build UI only); sub_833FF868 is what finalises the part (sub_822E0458, sub_822E14A0, sub_825A0E00, the current part
// sub_822E04D0), keeps it (sub_822D28C0), sets [root+4540] = 1 and closes the panel accepted (sub_833B3A60 -> sub_823BE7A0: +16 = 3,
// +20 = 1). The container then closes accepted (sub_833DFDC0) and the host keeps the superstar (sub_833BFA48 +C30 -> sub_822D28C0). Fix 2's
// walk of S (its ACCEPT row 6315) and the colour-variant second A are removed: one call ends every part kind, whatever stages it has.
// The panel is ready for the commit when the item is on and the panel idle in its list state: no put-on pending (P+984), no step (P+1032), no
// colour sub-mode (P+1028), the state switch on the list (P+992 0 or 8: both are the switch's idle case, sub_833FA688's jump table) and the
// preview ready.
inline bool KeepReady(uint32_t pending984, uint32_t step1032, uint32_t submode1028, uint32_t state992, bool preview_ready) {
  return pending984 == 0 && step1032 == 0 && submode1028 == 0 && (state992 == 0 || state992 == 8) && preview_ready;
}
// A page closed accepted: sub_823BE7A0 wrote +16 = 3 and +20 = 1 (a B's close writes 4 and 0, sub_823BE7D0).
inline bool ClosedAccepted(uint32_t state16, uint32_t result20) { return state16 == 3 && result20 == 1; }
// The end. The host drops overlay A and moves its page in one update (sub_833BFA48 +D4C..+E00: to the parent when the item page's kind
// vslot 52 is 2, to the grandparent when accepted and its code vslot 58 is 10 or 29), so once H+952 is 0 the page the host shows IS where
// the pick landed. Returns that page, or 0 while the overlay is up.
inline uint32_t CloseSettledPage(uint32_t overlay_a, uint32_t page) { return overlay_a ? 0 : page; }
enum class Landing : uint32_t { kNone = 0, kParent = 1, kGrandparent = 2, kItemPage = 3, kOther = 4, kEditPage = 5, kOpener = 6 };
inline Landing LandingOf(uint32_t page, uint32_t item_page, uint32_t parent, uint32_t grand) {
  if (page == parent) return Landing::kParent;
  if (page == grand) return Landing::kGrandparent;
  if (page == item_page) return Landing::kItemPage;
  return Landing::kOther;
}
inline const char* LandingName(Landing l) {
  switch (l) {
    case Landing::kParent: return "its parent";
    case Landing::kGrandparent: return "its grandparent";
    case Landing::kItemPage: return "the item page itself (the host stayed)";
    case Landing::kOther: return "neither its parent nor its grandparent";
    case Landing::kEditPage: return "the part's edit page (edit mode: the item is kept only when it closes accepted)";
    case Landing::kOpener: return "the page that opened the panel (the LOGOS page of a logo list)";
    default: return "nowhere yet";
  }
}

// The route states.
enum class Status : uint32_t { kIdle = 0, kWorking = 1, kApplied = 2, kStopped = 3 };
// Why it stopped or refused (cas_pick_reason). Routes compare these numbers: never renumber, only append.
enum class Reason : uint32_t {
  kNone = 0,
  kBadArg = 1,        // refused: the setting is not an item number
  kNotItemPage = 2,   // refused: the editor is not on an item page showing the parts panel
  kNotPlain = 3,      // refused: the panel's mode makes its A something else than a plain accept (PlainAccept)
  kOutOfRange = 4,    // refused: the panel lists fewer items
  kLocked = 5,        // refused: the item is locked ([[L+212] + 4 i] != 0): the panel neither puts it on nor accepts it
  kMessage = 6,       // refused: the panel's A would show a message box instead (its message words, or sub_8338FC20)
  kARefused = 7,      // refused: the A enable test is 0 on the item, or the item has no part (P+976 = -2), or the commit did not close it accepted
  kPanelBusy = 8,     // the parts panel or the preview did not become idle in time after arming
  kStepStuck = 9,     // a cursor step was not started or did not end in time
  kStepWrong = 10,    // a step landed elsewhere than the next or previous item, or the cursor left the item
  kApplyWait = 11,    // the panel did not put the item on the preview (or the preview did not settle) in time
  kCloseWait = 12,    // after the commit, the item page's overlay did not close and the editor did not settle in time
  kPanelGone = 13,    // the page host, the item page, its container or the panel went away while the pick ran
  kGotoArmed = 14,    // retired by S110 CASROW (a go-to armed while the pick drives is refused busy: src/cas_driver.h); kept so the numbers never move
  kEditPage = 15,     // edit mode (S110 CASROW): the panel's A did not start its edit stage (P+992 stayed off 2)
  kEditWait = 16,     // edit mode (S110 CASROW): the part's edit page did not come up idle, or the preview did not settle, in time
  kBusy = 17,         // refused: another driver (cas_goto / cas_row) holds the editor's presses (src/cas_driver.h)
};
inline constexpr const char* kReasonNames[] = {"none",       "bad_arg",    "not_item_page", "not_plain",  "out_of_range", "locked",
                                               "message",    "a_refused",  "panel_busy",    "step_stuck", "step_wrong",   "apply_wait",
                                               "close_wait", "panel_gone", "goto_armed",    "edit_page",  "edit_wait",    "busy"};
inline const char* ReasonName(Reason r) {
  const uint32_t i = uint32_t(r);
  return i < sizeof(kReasonNames) / sizeof(kReasonNames[0]) ? kReasonNames[i] : "?";
}

struct Report {
  Status status = Status::kIdle;
  uint32_t target = 0;  // the item asked for (the setting taken last)
  uint32_t item = 0;    // the item applied (0 until the pick ends applied)
  Reason reason = Reason::kNone;
  Landing page = Landing::kNone;  // where the editor settled after the applied pick
};
inline bool operator==(const Report& a, const Report& b) {
  return a.status == b.status && a.target == b.target && a.item == b.item && a.reason == b.reason && a.page == b.page;
}
inline Report Armed(uint32_t target) {
  Report r;
  r.status = Status::kWorking;
  r.target = target;
  return r;
}
inline void Applied(Report& r, Landing page) {
  r.status = Status::kApplied;
  r.item = r.target;
  r.reason = Reason::kNone;
  r.page = page;
}
inline void Halt(Report& r, Reason why) {
  r.status = Status::kStopped;
  r.item = 0;
  r.reason = why;
  r.page = Landing::kNone;
}

// The built-in state names (src/game_state.h AddHostStateSource). Field 0 is never used.
enum StateField : uint32_t { kFieldStatus = 1, kFieldItem, kFieldReason, kFieldPage, kFieldEnd };
struct StateName {
  const char* name;
  StateField field;
  const char* meaning;
};
inline constexpr StateName kStateNames[] = {
    {"cas_pick", kFieldStatus, "the CAS item pick (wwe2k14_cas_pick): 0 idle, 1 working, 2 applied, 3 stopped or refused (cas_pick_reason)"},
    {"cas_pick_item", kFieldItem, "the item the CAS pick applied, 1-based as the panel shows it (0 until it is applied)"},
    {"cas_pick_reason", kFieldReason, "why the CAS pick stopped or refused (src/cas_pick_core.h Reason; 0 none)"},
    {"cas_pick_page", kFieldPage,
     "where the editor settled after the applied CAS pick: 1 the item page's parent, 2 its grandparent, 3 the item page, 4 elsewhere, 5 the part's "
     "edit page (edit mode), 6 the LOGOS page that opened the logo list (0 until applied)"},
};
inline const StateName* FindStateName(const std::string& name) {
  for (const StateName& s : kStateNames)
    if (name == s.name) return &s;
  return nullptr;
}
inline uint32_t FieldValue(const Report& r, uint32_t field) {
  switch (field) {
    case kFieldStatus: return uint32_t(r.status);
    case kFieldItem: return r.item;
    case kFieldReason: return uint32_t(r.reason);
    case kFieldPage: return uint32_t(r.page);
    default: return 0;
  }
}

// A new request: the setting differs from the one taken last ("" = off). The same value set again is no change, so a route picks the same
// item again by setting "" first (as for cas_goto).
inline bool NewValue(const std::string& taken, const std::string& now) { return now != taken; }

}  // namespace wwe2k14::cas_pick
