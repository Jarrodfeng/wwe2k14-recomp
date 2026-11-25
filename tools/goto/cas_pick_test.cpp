// S110 CASPICK: host unit for src/cas_pick_core.h (the item argument, the step plan round a wrapping list, the step check, the A modes,
// the route states). Synthetic values only (no game data). Prints "cas_pick_test: ok N checks" and returns 0, else the failures and 1.
#include <cstdio>
#include <string>

#include "cas_goto_core.h"
#include "cas_pick_core.h"
#include "menu_focus.h"

using namespace wwe2k14::cas_pick;

static int g_n = 0, g_fail = 0;
static void Check(bool c, const char* what) {
  ++g_n;
  if (!c) {
    ++g_fail;
    std::printf("FAIL: %s\n", what);
  }
}

// The game's step, written from the listing and NOT from NextIndex (CASPICK review): the panel's step code P+1032 (1 or 2) is committed by
// sub_833FE7E8 as the list's vslot 5 sub_83431080 (code 1: [L+84] += 1, 0 when it reaches [L+20]) or vslot 6 sub_83431138 (code 2:
// [L+84] -= 1, [L+20] - 1 below 0); the highlighted index is sub_833F8B08: [L+84] + P+5184, brought into 0..[L+20]-1.
struct GameList {
  int top, off, n;  // [L+84], P+5184, [L+20]
};
static uint32_t GameIndex(const GameList& g) {
  const int v = g.top + g.off;
  return uint32_t(v >= g.n ? v - g.n : v < 0 ? v + g.n : v);
}
static void GameStep(GameList& g, uint32_t code) {
  if (code == 1 && ++g.top >= g.n) g.top = 0;
  if (code == 2 && --g.top < 0) g.top = g.n - 1;
}

int main() {
  // The argument: 1-based digits.
  uint32_t k = 0;
  Check(ParseItem("5", k).empty() && k == 5, "parse 5");
  Check(ParseItem("51", k).empty() && k == 51, "parse 51");
  Check(!ParseItem("0", k).empty() && !ParseItem("", k).empty() && !ParseItem("5a", k).empty() && !ParseItem("12345", k).empty() &&
            !ParseItem("-1", k).empty(),
        "0, empty, letters, too long, a sign: refused");

  // The plan: TOPS "1/51" -> item 5 is four downs; item 51 is one up (the list wraps); a tie goes down; out of range plans nothing.
  Plan p = PlanSteps(0, 5, 51);
  Check(p.dir == kDown && p.steps == 4, "1/51 -> 5: 4 down");
  p = PlanSteps(0, 51, 51);
  Check(p.dir == kUp && p.steps == 1, "1/51 -> 51: 1 up (wrap)");
  p = PlanSteps(10, 1, 51);
  Check(p.dir == kUp && p.steps == 10, "11/51 -> 1: 10 up");
  p = PlanSteps(4, 5, 51);
  Check(p.steps == 0, "already on 5: no step");
  p = PlanSteps(0, 3, 4);
  Check(p.dir == kDown && p.steps == 2, "a tie (0 -> index 2 of 4): down");
  p = PlanSteps(0, 52, 51);
  Check(p.steps == 0, "item 52 of 51: no plan");
  p = PlanSteps(0, 1, 0);
  Check(p.steps == 0, "an empty list: no plan");
  p = PlanSteps(50, 1, 51);
  Check(p.dir == kDown && p.steps == 1, "51/51 -> 1: 1 down (wrap)");

  // The direction codes are the game's: code 1 (kDown) moves the game's list to the next item, code 2 (kUp) to the previous one, both round
  // the ends. A walk driven by the plan through the game's own step reaches the item in exactly the planned number of steps, every step
  // landing where StepLanded expects, for any start, any P+5184 offset and both ways round.
  {
    GameList g{50, 0, 51};
    GameStep(g, uint32_t(kDown));
    Check(GameIndex(g) == 0 && StepLanded(50, 0, kDown, 51), "kDown from 51/51 is the game's step to 1/51 (wrap)");
    GameStep(g, uint32_t(kUp));
    Check(GameIndex(g) == 50 && StepLanded(0, 50, kUp, 51), "kUp from 1/51 is the game's step to 51/51 (wrap)");
  }
  bool walks_ok = true;
  for (int top : {0, 7, 50}) {
    for (int off : {0, 3, -2}) {
      for (uint32_t want : {1u, 5u, 26u, 51u}) {
        GameList g{top, off, 51};
        uint32_t cur = GameIndex(g), steps = 0;
        const uint32_t planned = PlanSteps(cur, want, 51).steps;
        for (; steps < 200; ++steps) {
          const Plan q = PlanSteps(cur, want, 51);
          if (q.steps == 0) break;
          GameStep(g, uint32_t(q.dir));
          const uint32_t now = GameIndex(g);
          if (!StepLanded(cur, now, q.dir, 51)) break;
          cur = now;
        }
        if (cur != want - 1 || steps != planned || planned > 25) walks_ok = false;
      }
    }
  }
  Check(walks_ok, "a walk through the game's step reaches the item in the planned steps (start, P+5184, item)");
  Check(StepLanded(3, 4, kDown, 51) && !StepLanded(3, 2, kDown, 51) && !StepLanded(3, 3, kDown, 51) && !StepLanded(3, 9, kDown, 51) &&
            StepLanded(0, 50, kUp, 51),
        "the step check: the asked neighbour only (a reversed, lost or far step is not landed)");

  // The A modes (sub_833FFE20 +230): 0 / 2 / 6 / 8 / 9 accept; 1 / 3 / 4 / 5 / 7 and anything past 9 are refused.
  bool modes_ok = true;
  for (uint32_t m = 0; m < 12; ++m) {
    const bool want = m == 0 || m == 2 || m == 6 || m == 8 || m == 9;
    if (PlainAccept(m) != want) modes_ok = false;
  }
  Check(modes_ok, "plain accept modes 0, 2, 6, 8, 9");
  Check(std::string(ModeName(3)).find("layered") != std::string::npos && std::string(ModeName(0)).find("accept") != std::string::npos &&
            std::string(ModeName(12)).find("outside") != std::string::npos,
        "mode names");

  // CASPICK fix 3 (rule 6, runs 281 / 294): the keep is the panel's own commit (vslot 6 sub_833FF868), called once the item is on and the
  // panel idle on its list. Run 294's MUSTACHE stop had the panel in state 3 (the colour stage): no commit there, and none while a step,
  // a put-on or a colour sub-mode is open, or the preview is loading.
  {
    Check(KeepReady(0, 0, 0, 0, true) && KeepReady(0, 0, 0, 8, true), "idle on the list (state 0, or 8 = the same idle case): commit");
    Check(!KeepReady(1, 0, 0, 0, true), "put-on pending (P+984): not yet");
    Check(!KeepReady(0, 1, 0, 0, true), "a cursor step running (P+1032): not yet");
    Check(!KeepReady(0, 0, 1, 0, true), "colour variants open (P+1028): not yet");
    Check(!KeepReady(0, 0, 0, 2, true) && !KeepReady(0, 0, 0, 3, true), "the edit stage (2) or its colour stage (3, run 294): no commit");
    Check(!KeepReady(0, 0, 0, 0, false), "preview loading: not yet");
    Check(ClosedAccepted(3, 1) && !ClosedAccepted(4, 0) && !ClosedAccepted(2, 1) && !ClosedAccepted(3, 0), "closed accepted: +16 = 3, +20 = 1");
    // The end: where the editor settled once overlay A is gone (the host drops it and moves its page in the same update, sub_833BFA48 +D4C..+E00).
    Check(LandingOf(0xA0, 0xB0, 0xA0, 0xC0) == Landing::kParent && LandingOf(0xC0, 0xB0, 0xA0, 0xC0) == Landing::kGrandparent &&
              LandingOf(0xB0, 0xB0, 0xA0, 0xC0) == Landing::kItemPage && LandingOf(0xD0, 0xB0, 0xA0, 0xC0) == Landing::kOther,
          "landings: parent 1, grandparent 2, the item page 3, elsewhere 4");
    Check(CloseSettledPage(0x1234, 0xA0) == 0 && CloseSettledPage(0x1234, 0xB0) == 0 && CloseSettledPage(0, 0xA0) == 0xA0 &&
              CloseSettledPage(0, 0xB0) == 0xB0,
          "the close settles only once overlay A is gone, on whatever page the host then shows");
  }

  // The route states: armed, applied, stopped; the item moves only on an applied pick.
  Report r = Armed(5);
  Check(FieldValue(r, kFieldStatus) == 1 && FieldValue(r, kFieldItem) == 0 && r.target == 5, "armed: working, no item yet");
  Applied(r, Landing::kParent);
  Check(FieldValue(r, kFieldStatus) == 2 && FieldValue(r, kFieldItem) == 5 && FieldValue(r, kFieldReason) == 0 && FieldValue(r, kFieldPage) == 1,
        "applied: item 5, settled on its parent");
  Report s = Armed(9);
  Halt(s, Reason::kLocked);
  Check(FieldValue(s, kFieldStatus) == 3 && FieldValue(s, kFieldItem) == 0 && FieldValue(s, kFieldReason) == 5 && FieldValue(s, kFieldPage) == 0,
        "stopped: 3, reason 5, no page");
  Check(!(r == s) && Report{} == Report{}, "reports compare");
  Check(std::string(ReasonName(Reason::kPanelGone)) == "panel_gone" && std::string(ReasonName(Reason::kNotPlain)) == "not_plain" &&
            std::string(ReasonName(Reason::kGotoArmed)) == "goto_armed" && uint32_t(Reason::kGotoArmed) == 14 &&
            std::string(ReasonName(Reason::kEditPage)) == "edit_page" && uint32_t(Reason::kEditPage) == 15 &&
            std::string(ReasonName(Reason::kEditWait)) == "edit_wait" && uint32_t(Reason::kEditWait) == 16 &&
            std::string(ReasonName(Reason(99))) == "?",
        "reason names");
  Check(sizeof(kReasonNames) / sizeof(kReasonNames[0]) == uint32_t(Reason::kBusy) + 1 && std::string(ReasonName(Reason::kBusy)) == "busy" &&
            uint32_t(Reason::kBusy) == 17,
        "a name for every reason (17 busy appended)");
  // S110 CASROW: the logo list's commit, and the pick's mode setting.
  Check(NestedLogoCommit(true, 3, 49, false) && NestedLogoCommit(true, 3, 50, false) && NestedLogoCommit(true, 3, 51, false), "the logo lists 49 / 50 / 51, nested, mode 3: committed");
  Check(!NestedLogoCommit(false, 3, 49, false) && !NestedLogoCommit(true, 3, 52, false) && !NestedLogoCommit(true, 0, 49, false) &&
            !NestedLogoCommit(true, 3, 49, true),
        "twins: mode 3 on the item page, a LETTERING category, another mode, edit mode: not committed");
  bool edit = true;
  Check(ParseMode("", edit).empty() && !edit && ParseMode("commit", edit).empty() && !edit && ParseMode("edit", edit).empty() && edit &&
            !ParseMode("Edit", edit).empty() && !edit,
        "the mode: empty / commit / edit; anything else refused");
  Check(LandingOf(0xA0, 0xB0, 0xA0, 0xC0) == Landing::kParent && uint32_t(Landing::kEditPage) == 5 && uint32_t(Landing::kOpener) == 6 &&
            std::string(LandingName(Landing::kOpener)).find("LOGOS") != std::string::npos,
        "landings 5 (the edit page) and 6 (the LOGOS page)");
  Check(NewValue("5", "3") && !NewValue("5", "5") && NewValue("5", "") && NewValue("", "5"), "a different value is a new request");

  // The names: found, fields 1..3, none of them a cas_goto or menu_* name.
  Check(FindStateName("cas_pick") && FindStateName("cas_pick")->field == kFieldStatus && FindStateName("cas_pick_item") &&
            FindStateName("cas_pick_page") && FindStateName("cas_pick_page")->field == kFieldPage && !FindStateName("cas_goto"),
        "names found");
  bool clash = false, fields_ok = true;
  for (const StateName& s2 : kStateNames) {
    if (s2.field == 0 || s2.field >= kFieldEnd) fields_ok = false;
    for (const wwe2k14::cas_goto::StateName& g : wwe2k14::cas_goto::kStateNames)
      if (std::string(s2.name) == g.name) clash = true;
    for (const wwe2k14::menu_focus::HostName& h : wwe2k14::menu_focus::kHostNames)
      if (std::string(s2.name) == h.name) clash = true;
  }
  Check(!clash && fields_ok && sizeof(kStateNames) / sizeof(kStateNames[0]) == kFieldEnd - 1, "cas_pick_* beside cas_goto / menu_*: no clash");

  if (g_fail) {
    std::printf("cas_pick_test: %d of %d checks FAILED\n", g_fail, g_n);
    return 1;
  }
  std::printf("cas_pick_test: ok %d checks\n", g_n);
  return 0;
}
