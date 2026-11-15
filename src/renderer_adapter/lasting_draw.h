// S102 T3 (a): a frame the sink would skip, whose lasting bind keeps more than about half of its draws, is drawn and shown instead.
// Pure decision (no GPU), so lasting_draw_test.cpp drives it. Switches: wwe2k14_rdr_lasting_draw (PerFrame, default off) and
// wwe2k14_rdr_lasting_draw_pct (PerFrame, default 50: the table holds integers, so the fraction 0.5 is written as a percent).
// What it trades (review F2; measured shapes from a Bryan window, estimates for the cost): a skipped frame that keeps 11202 of 20240 draws costs ~13.5 ms
// (the lasting bind) and shows nothing; drawn it costs ~22 ms (+63 %) and is shown. Net: about +5-12 % shown frames for about +3 % render-thread time,
// and a shown frame is at least max_queued frames old (latency), so the Surface job must report present gaps / latency as well as shown frames a second.
// Bound (review F1): nothing else limits the hand-off queue while frames are drawn instead, so a frame is drawn instead only while waiting <= max_queued.
#pragma once
#include <cstdint>

namespace rdr_adapter::lasting_draw {

struct Inputs {
  bool on = false;             // wwe2k14_rdr_lasting_draw (read per frame)
  int64_t pct = 50;            // wwe2k14_rdr_lasting_draw_pct (1..100; out of range is clamped)
  bool rate_skip = false;      // the 30 fps mode's skip is a rate, not an overload: never overridden
  bool lasting = false;        // the frame has lasting work (a lasting bind would have run)
  uint32_t draws = 0;          // draws in the frame
  uint32_t kept_draws = 0;     // draws the lasting bind would keep (lasting::Stats::kept_draws)
  uint32_t waiting = 0;        // frames waiting behind this one
  uint32_t max_queued = 2;     // the skip threshold (wwe2k14_rdr_max_queued)
};

// plant (tests only): 1 = ignores the threshold (always draws a lasting frame), 2 = >= instead of > (off by one at the boundary),
// 3 = overrides rate skips too, 5 = ignores the queue bound.
inline bool Decide(const Inputs& in, int plant = 0) {
  if (!in.on || !in.lasting) return false;
  if (in.rate_skip && plant != 3) return false;
  if (in.draws == 0) return false;
  if (in.waiting > in.max_queued && plant != 5) return false;   // F1: the queue is already past its bound: skip, never grow it
  if (plant == 1) return true;
  const int64_t pct = in.pct < 1 ? 1 : in.pct > 100 ? 100 : in.pct;
  const uint64_t lhs = uint64_t(in.kept_draws) * 100u, rhs = uint64_t(pct) * in.draws;
  return plant == 2 ? lhs >= rhs : lhs > rhs;   // "more than" the fraction
}

// What the sink does with a frame it wanted to skip (F4: live.cpp calls this, so the test drives the real rule).
//   draw        = take the draw path (bind every draw, show it)
//   owe_missed  = the frame's left-out per-frame copies are owed to the next drawn frame (NoteMissed). A frame drawn instead owes nothing: its own
//                 copies ran (owing them too would catch the state step up twice).
// plant 6 (tests only): a drawn-instead frame still owes its copies.
struct Verdict {
  bool draw = false;
  bool owe_missed = true;
};
inline Verdict Resolve(bool skip_wanted, const Inputs& in, int plant = 0) {
  Verdict v;
  if (!skip_wanted) {   // never a skipped frame: drawn, nothing owed
    v.draw = true;
    v.owe_missed = false;
    return v;
  }
  v.draw = Decide(in, plant);
  v.owe_missed = !v.draw || plant == 6;
  return v;
}

}  // namespace rdr_adapter::lasting_draw
