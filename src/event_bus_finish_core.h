// wwe2k14 - S110 ELIMFIN: the pure part of the bus's match decision lines `match what=finish | fall | scene` (docs\research\s110\ELIMFIN.md).
// No SDK, no guest reads. Unit: tools\bus\finish_core_test.cpp (py -I tools\bus\finish_core_selftest.py). Fed by src\event_bus.h Tick.
//
// The game's decision record: type G+472 (0x8376ACCC), winner / loser G+14584 / G+14585 (0x8376E3EC / 0x8376E3ED), G = 0x8376AAF4.
// After a decision the post-decision state sub_827C3660 either ends the match (sub_827C30B0, which writes the finish time G+14580 =
// 0x8376E3E8 at +BC) or, when rule byte G+13 (falls to win, against the winner's tally) or G+22 is set, may hand it to sub_827C2BE8,
// which clears the record (+440 / +47C) for the next fall. With G+13 and G+22 both 0 every decision ends the match (sub_827C3660
// +A8..+18C). In such a "multi-decision" rule the `finish` is said once the game commits the end (G+14580 written: sub_827C30B0 +BC,
// or sub_82679710 on the time-limit end, sub_827C2BE8 +2D8). G+14580 is cleared with the record by the apply (sub_822CA008 +A4) and
// the match reinit (sub_82770108: G+472..G+14891).
//
// S110 ELIMFIN fix 2 (run 293, Iron Man): a fall is NOT read from the record. In that run the record of each of 15 pinfalls was seen on
// one bus read only (the pending kickout was cancelled by it, src\event_bus.h MatchTick, yet no triple held 2 reads): sub_827C2BE8
// cleared it before the 2-read hold, so the record-held rule said no fall. The game counts a fall in its own per-slot tally, int16 at
// G+14588 + 2*slot (G + 2*(7294 + slot), slots 0..8): bumped for the winner slot by the decision evaluator sub_825B0EA0 +6B0, in the
// same call that writes the record (+5C4 / +668 / +670), and by the other decision writers (sub_825B15B0 +1C4 eliminations,
// sub_825B4A40 referee mode 3, sub_825B4F88 mode 4); read by sub_827C3660 +F8 (tally[winner] < G+13: more falls needed) and
// sub_8234D688 (the time-limit winner). Cleared with the record. So in a multi-decision rule each tally rise is one `fall` (scorer =
// the slot), and a decision the tally does not count (the time limit's type 9) is no line: the finish names the game's outcome.
#pragma once

#include <array>
#include <cstdint>

namespace wwe2k14::finish_core {

// lane 150: G+472 = 15 is the value a playing 30 Years scene leaves there (run 7092), not a match end: never a finish / fall
inline constexpr int64_t kSceneType = 15;
// the falls tally: int16 per slot at G + kTallyOffset + 2 * slot, slots 0..8 (G+14606, the next field, is the last fall's winner + 1)
inline constexpr int kTallySlots = 9;
inline constexpr uint32_t kTallyOffset = 14588;

// One read of the record, once per guest frame.
struct Read {
  int64_t type = 0;     // G+472 (int32)
  int64_t win = -1;     // G+14584 (signed byte)
  int64_t lose = -1;    // G+14585 (signed byte)
  bool multi = false;   // MultiDecision(G+13, G+22)
  bool ended = false;   // G+14580 != 0: the game committed the match end
  std::array<int64_t, kTallySlots> falls{};  // the tally per slot (int16)
};

enum class Line { kNone, kFinish, kFall, kScene };

struct State {
  int64_t seen = 0;        // the key of the last read (a line needs the same key 2 reads running)
  int64_t emitted = -1;    // the key last said; -1 after a 0 read (a new fall / match: the next decision is said again)
  bool end_said = false;   // a multi-decision rule's finish was said; cleared once G+14580 reads 0 (the next match)
  std::array<int64_t, kTallySlots> falls{};  // the tally at the last read
  bool falls_known = false;                  // false until the first read (the bus may start mid-match: no line for a tally already there)
};

// The slots whose tally rose at this read: one `fall` line each.
struct Scored {
  int n = 0;
  std::array<int, kTallySlots> slot{};
};

// sub_827C3660 +A8..+18C: the game may go on past a decision only when G+13 or G+22 is set
inline bool MultiDecision(uint8_t g13, uint8_t g22) { return g13 != 0 || g22 != 0; }

inline int64_t Key(const Read& r) { return r.type ? (r.type << 16) | ((r.win & 0xFF) << 8) | (r.lose & 0xFF) : 0; }

inline const char* What(Line l) { return l == Line::kFall ? "fall" : l == Line::kScene ? "scene" : "finish"; }

// The falls this read scored: in a multi-decision rule, every slot whose tally rose since the last read (a fall that ends the match
// rises too: its `fall` comes before the `finish`). Outside one nothing (the one decision is the finish; eliminations also bump the
// tally). A drop (the clear at the next match) only moves the baseline.
inline Scored Falls(State& s, const Read& r) {
  Scored out;
  if (s.falls_known && r.multi) {
    for (int i = 0; i < kTallySlots; ++i) {
      if (r.falls[i] > s.falls[i]) out.slot[out.n++] = i;
    }
  }
  s.falls = r.falls;
  s.falls_known = true;
  return out;
}

// The record line this read makes (kNone for most reads). Outside a multi-decision rule this is the bus's old rule unchanged: a
// non-zero triple held 2 reads is a finish, said again when it changes to another non-zero triple (the time limit fills the winner a
// frame later). In a multi-decision rule a held triple says nothing (the falls come from the tally: Falls), and the finish is said
// once, at the first read with G+14580 set.
inline Line Step(State& s, const Read& r) {
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
  return r.multi ? Line::kNone : Line::kFinish;
}

}  // namespace wwe2k14::finish_core
