// S110 FINPICK: the unit of src/finisher_pick_core.h (pure: no SDK, no game). Built and run by tools\bus\finisher_pick_selftest.py.
// The move ids are the player's own (docs\research\s110\SCENEMOM2.md claim 12): Bryan 324 slots 17 / 18 = 58010 Yes Lock / 42060; Lesnar 343 =
// 50413 F-5 / 58021 Kimura Lock; Cena 139 = 50326 AA / 58350 STF.
#include <cstdint>
#include <cstdio>
#include <string>

#include "finisher_pick_core.h"

using namespace wwe2k14::finisher_pick;

static int fails = 0, passes = 0;
static void Check(bool ok, const std::string& what) {
  (ok ? passes : fails)++;
  if (!ok) std::printf("FAIL: %s\n", what.c_str());
}

int main() {
  // the game's hold range, sub_82564D40: 58000 <= id < 58399
  Check(IsHold(58000) && IsHold(58398) && IsHold(58010) && IsHold(58350), "58000, 58398 and the holds read are holds");
  Check(!IsHold(57999) && !IsHold(58399) && !IsHold(50413) && !IsHold(42060) && !IsHold(0), "57999, 58399, F-5, the knee and 0 are not");
  Check(Fits(kHold, 58021) && !Fits(kHold, 50413) && Fits(kNoHold, 50413) && !Fits(kNoHold, 58021) && Fits(kAny, 58021) && Fits(kAny, 1), "Fits by the wish");

  // the pair edits
  Edit e = Decide(kNoHold, 17, 18, 50326, 58350);  // Cena, pin wanted: keep the AA (slot 17), drop the STF (slot 18)
  Check(e.applies && e.fit == 1 && !e.drop0 && e.drop1, "nohold, (17, 18) = (AA, STF): 262 over [r4+4]");
  e = Decide(kNoHold, 18, 17, 58350, 50326);       // the same pair offered 18 first (char+2648 != -1)
  Check(e.applies && e.fit == 1 && e.drop0 && !e.drop1, "nohold, (18, 17) = (STF, AA): 262 over [r4+0]");
  e = Decide(kHold, 17, 18, 58010, 42060);         // Bryan, submission wanted: keep the Yes Lock
  Check(e.applies && e.fit == 1 && !e.drop0 && e.drop1, "hold, (17, 18) = (Yes Lock, knee): 262 over [r4+4]");
  e = Decide(kNoHold, 17, 18, 58010, 42060);       // Bryan, pin wanted: keep the knee
  Check(e.applies && e.fit == 1 && e.drop0 && !e.drop1, "nohold, (17, 18) = (Yes Lock, knee): 262 over [r4+0]");
  e = Decide(kNoHold, 17, 18, 50413, 50413);       // both fit: the game's order stands
  Check(e.applies && e.fit == 1 && !e.drop0 && !e.drop1, "both fit: nothing dropped");
  e = Decide(kNoHold, 17, 18, 58010, 58021);       // neither fits: the pair stays, fit 0
  Check(e.applies && e.fit == 0 && !e.drop0 && !e.drop1, "neither fits: nothing dropped, fit 0");
  e = Decide(kAny, 17, 18, 50326, 58350);
  Check(!e.applies && e.fit == -1 && !e.drop0 && !e.drop1, "no wish: no edit");
  e = Decide(kHold, 19, 19, 58010, 58010);
  Check(!e.applies && !e.drop0 && !e.drop1, "the (19, 19) pair is not touched");
  e = Decide(kHold, 15, 16, 58010, 42060);
  Check(!e.applies && !e.drop0 && !e.drop1, "the signature pair (15, 16) is not touched");
  e = Decide(kHold, 17, 17, 58010, 42060);
  Check(!e.applies, "(17, 17) is not the pair");
  Check(kNone == 262, "the chooser's 'none' is 262");
  // whose moves the chooser tests (sub_82605418 +70..+98): the character's own; [char+2672]'s when the chooser context's word +80 is set
  Check(MoveOwner(0, 0x40001000u, 0x40002000u) == 0x40001000u && MoveOwner(1, 0x40001000u, 0x40002000u) == 0x40002000u, "the move owner follows ctx+80");

  // the store and the host state
  Check(g_wishes.load() == 0 && FitState(0) == -1 && FitState(8) == -1 && FitState(9) == -1 && FitState(-1) == -1, "no wish at start: -1");
  std::string t = SetWish(0, kNoHold, 50326, 58350);
  Check(FitState(0) == 1 && g_wishes.load() == 1 && t.find("50326 / 58350") != std::string::npos && t.find("fits") != std::string::npos, "nohold on Cena: fits (" + t + ")");
  t = SetWish(1, kNoHold, 58010, 58021);
  Check(FitState(1) == 0 && g_wishes.load() == 2 && t.find("no nohold finisher") != std::string::npos, "nohold with two holds: 0 (" + t + ")");
  t = SetWish(1, kAny, 0, 0);
  Check(FitState(1) == -1 && g_wishes.load() == 1, "any clears the seat's wish");
  t = SetWish(9, kHold, 1, 2);
  Check(t == "no such seat" && g_wishes.load() == 1, "seat 9 refused");
  g_told[0].store(true);
  Clear();
  Check(g_wishes.load() == 0 && FitState(0) == -1 && g_wish[0].load() == kAny && !g_told[0].load(), "Clear: every wish and the told flag go");

  // S110 TOOLFIT: the posture families (sub_82602DE0's six id ranges, each `>= first` at +30, `< end` at +44 of its predicate)
  Check(FamilyOf(58099) == nullptr && FamilyOf(58100) && FamilyOf(58100)->r21 == 3 && FamilyOf(58148) && FamilyOf(58149) == nullptr,
        "58100..58148: r21 3; 58099 and 58149 in none");
  Check(FamilyOf(58150) && FamilyOf(58150)->r21 == 4 && FamilyOf(58300) && FamilyOf(58300)->r21 == 4 && FamilyOf(58348) && FamilyOf(58349) == nullptr,
        "58150.. and 58300..58348: r21 4; 58349 in none");
  Check(FamilyOf(58200) && FamilyOf(58200)->r21 == 5 && FamilyOf(58250) && FamilyOf(58250)->r21 == 3, "58200..: r21 5; 58250..: r21 3");
  Check(FamilyOf(58350) && FamilyOf(58350)->r21 == 5 && std::string(FamilyOf(58350)->test) == "+1CC0" && FamilyOf(58398) && FamilyOf(58399) == nullptr,
        "58350..58398: r21 5 at +1CC0");
  Check(FreeHold(58000) && FreeHold(58010) && FreeHold(58021) && FreeHold(58149) && !FreeHold(58350) && !FreeHold(58100) && !FreeHold(50326) &&
            !FreeHold(58399),
        "family-free holds: 58000..58099 and the gaps; not the STF, not a non-hold");
  e = Decide(kHold, 17, 18, 50326, 58350);  // Cena, submission wanted: the STF is kept, but only a family hold fits (run 289)
  Check(e.applies && e.fit == kFitFamily && e.drop0 && !e.drop1, "hold, (AA, STF): the STF kept, fit 2");
  e = Decide(kHold, 17, 18, 58010, 58350);  // a family-free hold beside a family one: fits, nothing dropped
  Check(e.applies && e.fit == 1 && !e.drop0 && !e.drop1, "hold, (Yes Lock, STF): fit 1");
  e = Decide(kNoHold, 17, 18, 50326, 58350);
  Check(e.fit == 1 && e.drop1, "nohold is not judged by family");
  t = SetWish(2, kHold, 50326, 58350);
  Check(FitState(2) == kFitFamily &&
            t == "slots 17 / 18 = 50326 / 58350: a hold finisher fits only in a posture family (50326 not a hold; 58350 a hold in posture family "
                 "58350..58398 (sub_82602DE0 +1CC0, r21 5)): no family-free hold (ids 58000..58099) to keep",
        "hold on Cena: 2, the family named (" + t + ")");
  t = SetWish(3, kHold, 58021, 50413);
  Check(FitState(3) == 1 && t == "slots 17 / 18 = 58021 / 50413: a hold finisher fits", "hold on Lesnar: 1 (" + t + ")");
  t = SetWish(4, kAny, 58021, 50413);
  Check(FitState(4) == -1 && t == "no wish: the game's own choice", "any: no wish, its answer unchanged (" + t + ")");
  Check(ClassText(58021) == "58021 a family-free hold" && ClassText(50413) == "50413 not a hold" &&
            ClassText(58150) == "58150 a hold in posture family 58150..58198 (sub_82602DE0 +1BFC, r21 4)",
        "ClassText: free / not a hold / family");
  Clear();

  // the state names
  Check(StateSeat("finisher_fit_0") == 0 && StateSeat("finisher_fit_8") == 8, "finisher_fit_0 / _8");
  Check(StateSeat("finisher_fit_9") == -1 && StateSeat("finisher_fit_") == -1 && StateSeat("finisher_fit_00") == -1 && StateSeat("finisher_fit_x") == -1 &&
            StateSeat("finisher_fat_0") == -1,
        "not a state name");

  // the log line (the brief's words)
  Check(NoFitText(0, kHold, 50326, 58350) == "seat 0: no hold finisher in slots 17/18 (ids 50326, 58350): the game's choice stands", "the no-fit line");
  Check(NoFitText(1, kNoHold, 58010, 58021) == "seat 1: no nohold finisher in slots 17/18 (ids 58010, 58021): the game's choice stands", "the no-fit line, nohold");

  std::printf("SUMMARY %d passed, %d failed\n", passes, fails);
  return fails ? 1 : 0;
}
