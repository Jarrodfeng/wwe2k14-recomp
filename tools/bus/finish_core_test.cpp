// S110 ELIMFIN: the unit of src/event_bus_finish_core.h (pure: no SDK, no game). Built and run by tools\bus\finish_core_selftest.py.
// OldStep is the bus's rule before ELIMFIN (src/event_bus.h Tick at f2d6fd756) and ElimfinStep the first ELIMFIN rule (a held record in a
// multi-decision rule is a fall; main at 48da259f4), kept here only to show the cases each one gets wrong.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "event_bus_finish_core.h"

using namespace wwe2k14::finish_core;

static int fails = 0;
static void Check(bool ok, const std::string& what) {
  if (!ok) {
    ++fails;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// the old rule: a line for every held new non-zero triple; 15 is the scene line
struct Old {
  int64_t seen = 0, emitted = -1;
};
static Line OldStep(Old& s, const Read& r) {
  const int64_t key = Key(r);
  if (key != s.seen) {
    s.seen = key;
  } else if (key != 0 && key != s.emitted) {
    s.emitted = key;
    return r.type == kSceneType ? Line::kScene : Line::kFinish;
  } else if (key == 0) {
    s.emitted = -1;
  }
  return Line::kNone;
}

// the first ELIMFIN rule: in a multi-decision rule a record held 2 reads is a fall (run 293: each pinfall's record lived one read)
static Line ElimfinStep(State& s, const Read& r) {
  const int64_t key = Key(r);
  if (!r.ended) s.end_said = false;
  if (r.multi && r.ended && key != 0 && r.type != kSceneType) {
    s.seen = key;
    s.emitted = key;
    if (s.end_said) return Line::kNone;
    s.end_said = true;
    return Line::kFinish;
  }
  if (key != s.seen) {
    s.seen = key;
    return Line::kNone;
  }
  if (key == 0) {
    s.emitted = -1;
    return Line::kNone;
  }
  if (key == s.emitted) return Line::kNone;
  s.emitted = key;
  if (r.type == kSceneType) return Line::kScene;
  return r.multi ? Line::kFall : Line::kFinish;
}

// a read: the record, the rule, the end word and the tally of slots 0 / 1 (G+14588 / G+14590)
static Read R(int64_t type, int64_t win, int64_t lose, bool multi, bool ended, int64_t t0 = 0, int64_t t1 = 0) {
  Read r;
  r.type = type;
  r.win = win;
  r.lose = lose;
  r.multi = multi;
  r.ended = ended;
  r.falls[0] = t0;
  r.falls[1] = t1;
  return r;
}

// one line of a run: the what, and the scorer slot for a fall (-1 otherwise)
struct Out {
  Line l;
  int slot;
  bool operator==(const Out& o) const { return l == o.l && slot == o.slot; }
};
static const Out F0{Line::kFall, 0}, F1{Line::kFall, 1}, FIN{Line::kFinish, -1}, SCENE{Line::kScene, -1};

// the lines a read sequence makes, as src/event_bus.h Tick says them: the falls first, then the record's line
static std::vector<Out> Run(const std::vector<Read>& rs) {
  State s;
  std::vector<Out> out;
  for (const Read& r : rs) {
    const Scored sc = Falls(s, r);
    for (int i = 0; i < sc.n; ++i) out.push_back({Line::kFall, sc.slot[size_t(i)]});
    const Line l = Step(s, r);
    if (l != Line::kNone) out.push_back({l, -1});
  }
  return out;
}
static std::vector<Out> RunOld(const std::vector<Read>& rs) {
  Old s;
  std::vector<Out> out;
  for (const Read& r : rs) {
    const Line l = OldStep(s, r);
    if (l != Line::kNone) out.push_back({l, -1});
  }
  return out;
}
static std::vector<Out> RunElimfin(const std::vector<Read>& rs) {
  State s;
  std::vector<Out> out;
  for (const Read& r : rs) {
    const Line l = ElimfinStep(s, r);
    if (l != Line::kNone) out.push_back({l, l == Line::kFall ? int(r.win) : -1});
  }
  return out;
}
static std::vector<Read> Rep(std::vector<Read> v, const Read& r, int n) {
  for (int i = 0; i < n; ++i) v.push_back(r);
  return v;
}

int main() {
  using L = std::vector<Out>;
  // the rule bytes: sub_827C3660 +A8..+18C; the tally: int16 at G + 2 * (7294 + slot) (sub_825B0EA0 +69C..+6B0)
  Check(!MultiDecision(0, 0) && MultiDecision(2, 0) && MultiDecision(0, 1) && MultiDecision(3, 1), "MultiDecision = G+13 || G+22");
  Check(kTallyOffset == 2 * 7294 && kTallySlots == 9 && kTallyOffset + 2 * kTallySlots == 14606, "the tally is G+14588 + 2*slot, slots 0..8, up to G+14606");
  Check(std::string(What(Line::kFall)) == "fall" && std::string(What(Line::kFinish)) == "finish" && std::string(What(Line::kScene)) == "scene",
        "the what words");

  // 1. a one-fall rule (G+13 = G+22 = 0: every capture in out\scenarios): the old rule, read for read; the tally rises at the one
  //    decision (sub_825B0EA0 +6B0) and makes no line
  std::vector<Read> one;
  one = Rep(one, R(0, -1, -1, false, false), 3);
  one = Rep(one, R(1, 1, 0, false, false, 0, 1), 4);   // the pinfall (sub_825B0EA0 +5C4) and slot 1's tally
  one = Rep(one, R(1, 1, 0, false, true, 0, 1), 30);   // the end path writes G+14580 later (sub_827C30B0 +BC): no second line
  Check(Run(one) == L{FIN} && RunOld(one) == L{FIN}, "one-fall pinfall: one finish, as before (no fall from the tally)");
  Check(Run(Rep({}, R(1, 1, 0, false, false), 1)).empty(), "one read is not held: no line");
  // the time limit fills the winner a frame later: said again on the changed triple (unchanged)
  std::vector<Read> tl;
  tl = Rep(tl, R(9, -1, -1, false, false), 2);
  tl = Rep(tl, R(9, 0, 1, false, false), 2);
  Check(Run(tl) == L{FIN, FIN} && RunOld(tl) == Run(tl), "one-fall time limit: the two finishes as before");
  // an elimination: no record (sub_825B0EA0 +468..+570), the eliminator's tally rises (sub_825B15B0 +1C4): no line outside a multi rule
  std::vector<Read> elim;
  elim = Rep(elim, R(0, -1, -1, false, false), 5);
  elim = Rep(elim, R(0, -1, -1, false, false, 1, 0), 50);
  Check(Run(elim).empty(), "an elimination in a one-fall rule: no line");

  // 2. two-out-of-three falls (G+13 = 2), the record held through the post-decision wait: a fall per tally rise, then the finish
  std::vector<Read> f3;
  f3 = Rep(f3, R(0, -1, -1, true, false), 3);
  f3 = Rep(f3, R(1, 1, 0, true, false, 0, 1), 120);    // fall 1 (record and tally in one call)
  f3 = Rep(f3, R(0, -1, -1, true, false, 0, 1), 200);  // the record cleared (sub_827C2BE8 +440 / +47C): the match goes on
  f3 = Rep(f3, R(6, 1, 0, true, false, 0, 2), 120);    // fall 2 (a submission)
  f3 = Rep(f3, R(6, 1, 0, true, true, 0, 2), 60);      // the end committed: G+14580 written
  Check(Run(f3) == L{F1, F1, FIN}, "2-of-3 falls: fall, fall, finish");
  Check(RunOld(f3) == L{FIN, FIN}, "OLD CODE FAILS: it says finish at the first fall (the match goes on)");
  const std::vector<Read> first = Rep({}, R(1, 1, 0, true, false, 0, 1), 2);
  Check(RunOld(first) == L{FIN} && Run(first).empty(), "OLD CODE FAILS: a non-final fall held 2 reads is a finish (here: no line, the tally did not move)");

  // 3. run 293 (Iron Man, #139 vs Orton): each pinfall's record lived ONE read (it cancelled the pending kickout, then sub_827C2BE8
  //    cleared it), the tally rose with it; at the time limit the record held 9 for 379 frames, then 1 + G+14580 (sub_827C2BE8 +2C8 / +2D8)
  std::vector<Read> iron;
  iron = Rep(iron, R(0, -1, -1, true, false), 50);
  for (int k = 1; k <= 15; ++k) {
    iron = Rep(iron, R(1, 0, 1, true, false, k, 0), 1);
    iron = Rep(iron, R(0, -1, -1, true, false, k, 0), 300);
  }
  iron = Rep(iron, R(9, 0, -1, true, false, 15, 0), 379);
  iron = Rep(iron, R(1, 0, 1, true, true, 15, 0), 30);
  L want(15, F0);
  want.push_back(FIN);
  Check(Run(iron) == want, "run 293: 15 falls by slot 0, then one finish");
  Check(RunElimfin(iron) == L{F0, FIN}, "ELIMFIN CODE FAILS (run 293): no fall for 15 one-read records, a fall for the time limit");
  //    a draw at the time limit (no tally winner, sub_827C2BE8 +2D4): the record stays 9 and the end is committed on it
  std::vector<Read> draw;
  draw = Rep(draw, R(9, -1, -1, true, false, 1, 1), 60);
  draw = Rep(draw, R(9, -1, -1, true, true, 1, 1), 30);
  Check(Run(draw) == L{FIN}, "a draw: the finish only (the time limit is no fall: the tally does not move)");

  // 4. the end committed in the same frame as the deciding fall (Inferno sub_825B0638, type 11 sub_825B1A28): the fall comes first
  std::vector<Read> same;
  same = Rep(same, R(0, -1, -1, true, false, 1, 0), 2);
  same = Rep(same, R(10, 0, 1, true, true, 2, 0), 10);
  Check(Run(same) == L{F0, FIN}, "decision and end in one frame: fall, then finish");
  Check(Run(Rep(Rep({}, R(0, -1, -1, true, false), 2), R(10, 0, 1, true, true), 10)) == L{FIN}, "an end the tally does not count: finish only");

  // 5. after the end, the record cleared and put back while G+14580 holds (sub_827BF118 / sub_827BEBE0): no second finish
  std::vector<Read> back = f3;
  back = Rep(back, R(0, -1, -1, true, true, 0, 2), 10);
  back = Rep(back, R(6, 1, 0, true, true, 0, 2), 10);
  Check(Run(back) == L{F1, F1, FIN}, "the record back after the end: no second finish");

  // 6. the next match (the apply clears G+472..G+14891: the tally drops to 0, no line), its falls and finish are said again
  std::vector<Read> next = f3;
  next = Rep(next, R(0, -1, -1, true, false), 10);
  next = Rep(next, R(1, 0, 1, true, false, 1, 0), 3);
  next = Rep(next, R(1, 0, 1, true, true, 1, 0), 3);
  Check(Run(next) == L{F1, F1, FIN, F0, FIN}, "the next match: fall, finish again; the cleared tally says nothing");
  //    the same slot scores again in the next match: its first fall counts from the cleared 0, not from last match's 2
  std::vector<Read> again = f3;
  again = Rep(again, R(0, -1, -1, true, false), 10);
  again = Rep(again, R(1, 1, 0, true, false, 0, 1), 3);
  Check(Run(again) == L{F1, F1, FIN, F1}, "the next match: slot 1's first fall again (the baseline follows the clear)");

  // 7. the bus turned on mid-match: a tally already there is no fall; the next rise is
  std::vector<Read> mid;
  mid = Rep(mid, R(0, -1, -1, true, false, 2, 1), 10);
  mid = Rep(mid, R(1, 1, 0, true, false, 2, 2), 1);
  Check(Run(mid) == L{F1}, "bus on mid-match: only the rise after the first read");

  // 8. the 30 Years scene value: scene, whatever the rule bytes and G+14580 (its writers sub_827BE558 / sub_827C1AC8 write both)
  Check(Run(Rep({}, R(15, -1, -1, true, true), 5)) == L{SCENE}, "type 15 in a multi rule with G+14580: scene");
  const std::vector<Read> sc1 = Rep({}, R(15, -1, -1, false, false), 5);
  Check(Run(sc1) == L{SCENE} && RunOld(sc1) == L{SCENE}, "type 15: scene, as before");

  std::printf("finish_core unit: %d failures\n", fails);
  return fails ? 1 : 0;
}
