// S110 OMGVERB: the unit of src/omg_wish_core.h (pure: no SDK, no game). Built and run by tools\bus\omg_wish_selftest.py.
#include <cstdint>
#include <cstdio>
#include <string>

#include "omg_wish_core.h"

using namespace wwe2k14::omg_wish;

static int fails = 0, passes = 0;
static void Check(bool ok, const std::string& what) {
  (ok ? passes : fails)++;
  if (!ok) std::printf("FAIL: %s\n", what.c_str());
}

static Gates All() {
  Gates g;
  g.able = g.rules = g.ring = g.latch = g.stock = true;
  return g;
}
static bool Has(const std::string& t, const char* s) { return t.find(s) != std::string::npos; }

int main() {
  // the constants as read (sub_825FCEB8 / sub_826056E0 / sub_82601228)
  Check(kRingAct == 2101 && kFinisherKind == 1 && kKindOff == 24 && kPressOff == 48 && kMoveOff == 92 && kCandSize == 124, "the offsets and the act");
  Check(kReqKindOff == 28 && kReversedOff == 40 && kSlotNoOff == 88 && kReqStateOff == 116 && kRequestKind == 4 && kRequestState == 839 && kFollowUpKind == 8,
        "the request's fields (+28 4, +116 839), the reversal flag +40, the slot +88, the follow-up kind 8");
  Check(kNotOurs == 2 && kNoPress == 1, "the test's results: 2 a gate failed, 1 no press");

  // when the hook presses
  Check(ShouldPress(true, 1, 0), "a wish, the finisher candidate, no press: press");
  Check(!ShouldPress(false, 1, 0), "no wish: never");
  Check(!ShouldPress(true, 2, 0) && !ShouldPress(true, 4, 0) && !ShouldPress(true, 0, 0), "a signature (2) / slot-14 (4) / other candidate: never");
  Check(!ShouldPress(true, 1, 1), "a press already there (the follow-up's, or the player's): left alone");
  // S110 OMGVERB review: the chooser's own press rule (sub_826056E0 +A4..+EC): the hook presses only where the attacker's own press would count
  Check(PressCounts(false, false, false), "no filter holds: a press of the attacker's would count");
  Check(!PressCounts(true, false, false), "the attacker's act in sub_82564518's set: the chooser clears a press (+C4)");
  Check(!PressCounts(false, true, false), "sub_8256BD30 holds: the chooser clears a press (+C4)");
  Check(!PressCounts(false, false, true), "an FRA follow-up: the press is its own [fra+224] (0 until its step), never the pad's (+DC..+EC)");

  // what is kept: the request (+3AC..+3D8), nothing else
  Check(Keep(0, 2101, 4, 839), "result 0, act 2101, kind 4, state 839: the game made the ring-break request");
  Check(!Keep(0, 2150, 4, 839) && !Keep(0, 50379, 4, 537), "result 0 with another move (a superplex; the 537 request with the slot's own move): undone");
  Check(!Keep(0, 2101, 0, 839) && !Keep(0, 2101, 4, 0), "2101 without the request's kind / state: not the request path, undone");
  Check(!Keep(2, 2101, 4, 839) && !Keep(1, 2101, 4, 839), "a gate failed / no press: undone");
  Check(Unpressed(2) == 2 && Unpressed(0) == 1 && Unpressed(1) == 1, "the undone result is the no-press one");

  // S110 OMGVERB fix 2: the follow-up's pass (an FRA of kind 8, its own press)
  Check(Classify(0, 2101, 0) == kBreak, "result 0, 2101, no reversal: the break started (sub_82595288, +410..+444)");
  Check(Classify(0, 2101, 1) == kReversed, "result 0, 2101, +40 1: reversed (1250, +400..+408)");
  Check(Classify(2, 50379, 0) == kRefused, "result 2: a gate refused on the second pass (run 315: the slot's ordinary finisher 50379 followed)");
  Check(Classify(0, 50379, 0) == kRefused, "result 0 with the slot's own move (the target standing up, 151 -> the 537 request): refused, not a break");
  Check(Classify(1, 2101, 0) == kRefused, "no press seen (1): refused, never a break");
  Check(IsLost(kRequested, 100, 105, false), "requested, a test 5 frames later with no FRA of kind 8: the follow-up is gone");
  Check(!IsLost(kRequested, 100, 100, false), "the same frame (the other slot's test, before the executor makes the FRA): not lost");
  Check(!IsLost(kRequested, 100, 105, true), "the FRA of kind 8 still there (its wait): not lost");
  Check(!IsLost(kArmed, 100, 105, false) && !IsLost(kBreak, 100, 105, false), "only a kept request can be lost");

  // the gates
  Check(FitOf(All()) == 1, "every gate: armed");
  for (int k = 0; k < 5; ++k) {
    Gates g = All();
    bool* f[] = {&g.able, &g.rules, &g.ring, &g.latch, &g.stock};
    *f[k] = false;
    Check(FitOf(g) == 0, "gate " + std::to_string(k) + " off: not armed");
  }
  {
    Gates g = All();
    g.able = false;
    g.stock = false;
    const std::string t = GatesText(g, 1, 3);
    Check(Has(t, "class NO") && Has(t, "stock 1 of 3 (NOT ENOUGH)"), "the gates text names the class and the stock: " + t);
  }

  // the store and the host state
  Check(g_wishes.load() == 0 && FitState(0) == -1 && FitState(8) == -1 && FitState(9) == -1 && FitState(-1) == -1, "nothing set at start: -1");
  std::string t = SetWish(0, true, All(), 3, 3);
  Check(FitState(0) == kArmed && g_wishes.load() == 1 && g_wish[0].load() && t.find("armed: ") == 0, "an armed seat: 1 (" + t + ")");
  Gates no = All();
  no.able = false;
  t = SetWish(1, true, no, 3, 3);
  Check(FitState(1) == 0 && !g_wish[1].load() && g_wishes.load() == 1 && t.find("not armed: class NO") == 0, "a failed gate: 0 and no wish (" + t + ")");
  Check(NotePressed(0, 12250) && FitState(0) == kRequested && g_wish[0].load() && g_req_frame[0].load() == 12250, "the kept request: 2, logged, still looked at");
  Check(!NotePressed(0, 12250) && FitState(0) == kRequested, "the other slot's request in the same pass: still 2, not logged again");
  t = SetWish(0, false, Gates{}, 0, 0);
  Check(FitState(0) == -1 && !g_wish[0].load() && g_wishes.load() == 0 && t.find("no wish") == 0, "off: -1 and the count drops");
  t = SetWish(9, true, All(), 3, 3);
  Check(t == "no such seat" && g_wishes.load() == 0, "seat 9 refused");

  // S110 run 315 as a case: armed -> the request kept on slot 17 (move 50379) -> the follow-up's pass refused -> state 5, the hook stops
  SetWish(0, true, All(), 3, 3);
  Check(NotePressed(0, 12250) && FitState(0) == kRequested, "run 315: the request");
  {
    Readings r;
    r.legal = 0;
    r.state = 839;
    r.act = 74;
    r.target_act = 150;
    r.distance = 148.0f;
    r.stock = 3;
    r.cost = 3;
    const State s = Classify(2, 50379, 0);
    NoteEnd(0, s);
    const std::string line = FollowUpText(0, s, 2, 50379, r);
    Check(FitState(0) == kRefused && !g_wish[0].load() && g_wishes.load() == 0, "run 315: the follow-up refused: 5, the seat no longer looked at");
    Check(Has(line, "REFUSED") && Has(line, "ordinary finisher") && Has(line, "attacker legal 0 (state 839, act 74)") && Has(line, "target act 150") &&
              Has(line, "distance 148.0") && Has(line, "stock 3 of 3"),
          "run 315: the refused line names the gate inputs: " + line);
  }
  // the other ends
  SetWish(2, true, All(), 3, 3);
  NotePressed(2, 500);
  NoteEnd(2, Classify(0, 2101, 0));
  Check(FitState(2) == kBreak && !g_wish[2].load(), "a follow-up that starts the break: 3 (what the route waits for)");
  Check(Has(FollowUpText(2, kBreak, 0, 2101, Readings{}), "started the break (sub_82595288)"), "the break line");
  Check(Has(FollowUpText(2, kReversed, 0, 2101, Readings{}), "REVERSED"), "the reversal line");
  SetWish(3, true, All(), 3, 3);
  NotePressed(3, 500);
  Check(IsLost(FitState(3), g_req_frame[3].load(), 530, false), "a later test without the FRA");
  NoteEnd(3, kLost);
  Check(FitState(3) == kLost && !g_wish[3].load(), "lost: 6");
  Check(Has(LostText(3, 30, 74, 151), "GONE without its test") && Has(LostText(3, 30, 74, 151), "30 frames") && Has(LostText(3, 30, 74, 151), "target act 151"),
        "the lost line names the gap and the acts");
  Check(Has(RequestText(0, 17, 50379), "REQUEST (act 2101, kind 4, state 839) at the slot-17 finisher candidate (its own move 50379)") &&
            Has(RequestText(0, 17, 50379), "the break starts only on its follow-up's pass") && !Has(RequestText(0, 17, 50379), "chose act 2101"),
        "the request line says what happened, not that the game chose the break");

  SetWish(4, true, All(), 3, 3);
  NotePressed(4, 7);
  Clear();
  Check(g_wishes.load() == 0 && FitState(1) == -1 && FitState(2) == -1 && FitState(4) == -1 && !g_wish[4].load() && !g_told[4].load() && g_req_frame[4].load() == 0,
        "Clear: every wish, state, told flag and request frame go");
  Check(NotePressed(5, 1) && FitState(5) == kRequested, "a note on a seat after Clear sets its state");
  Clear();
  Check(FitState(5) == -1, "Clear also clears a state without a wish");

  // the state names
  Check(StateSeat("omg_fit_0") == 0 && StateSeat("omg_fit_8") == 8, "omg_fit_0 / _8");
  Check(StateSeat("omg_fit_9") == -1 && StateSeat("omg_fit_") == -1 && StateSeat("omg_fit_00") == -1 && StateSeat("omg_fix_0") == -1, "not a state name");

  std::printf("SUMMARY %d passed, %d failed\n", passes, fails);
  return fails ? 1 : 0;
}
