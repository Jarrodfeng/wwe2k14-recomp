// The crowd's far cards, thinned at record time, game side (Session 101, lane 175; docs\renderer\SKIP_GAME_WORK_MAP.md section 13). Decision logic: card_thin_core.h (unit-tested by
// tools\crowd\card_thin_test.cpp). This file is the glue and nothing else: no function of the game is replaced, no instruction of it, no guest memory is read or written.
//
// Three observers, all in hooks that already exist (a function takes one hook): the crowd draw entry sub_82519508 (crowd_hooks.h) starts a RUN; the card loop sub_82512F70 (scene_crowd.h) starts a
// TYPE LOOP; the op-15 recorder sub_82BB10C0 (scene_crowd.h), for the card draw's call site only (return address 0x825130C8 = the `bl` at 0x825130C4), asks DropCard(). Dropping is "return without
// calling the recorder": the record is not written, which is the same thing the P2b hook does for the blocks. The card's op-21 (sub_82BB27D8 at 0x825130AC, c76-c78) is NEVER touched: the register
// file, the scene record's feed (crowd_hooks.h OnOp21) and the replay's bookkeeping stay the game's.
//
//   --wwe2k14_rdr_crowd_card_thin=0 (default)  the hooks call the original and nothing else; the observers do one cvar read.
//   =1  CENSUS: the decision is made and counted, nothing is dropped. The live check's first step: what mode 2 would have dropped, how often a proof run came, whether the gate ever proved.
//   =2  SKIP: as built, behaves as 1 (kSkipBuilt = false). The main session flips kSkipBuilt after reading a census from a milestone run (SKIP_GAME_WORK_MAP 13.7). Inert by construction until then.
// The binder side (crowd_cards_core.cpp's thin branch) is compiled either way and acts only on a frame whose stream holds fewer card draws than the record has cards; a frame the game recorded in
// full is never touched. With the cut inert no frame is thin, so nothing changes.
// Not for a determinism run (the records change: the det hash), not for MSAA / predicated tiling (a tiled frame runs the crowd draw once per tile: a run would be part proof, part thin).
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../frame_stats.h"   // wwe2k14::g_guest_frame (the stamp of the per-frame tally)
#include "card_thin_core.h"

REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_bake_skip);   // lane 155 (bake_skip_hook.h): review S6

REXCVAR_DEFINE_INT32(wwe2k14_rdr_crowd_card_thin, 0, "wwe2k14",
                     "Session 101 lane 175: the far cards' thin record. 0 off; 1 census (decide and count what would be dropped, touch nothing); 2 drop the op-15 draw of every far card except the first "
                     "of each person type's loop while the binder has proved ours draws the cards from the scene record (one run in 32 is recorded in full as the proof). As built 2 behaves as 1 until the "
                     "census has been read from a milestone run. Needs --wwe2k14_rdr_crowd_cards (and so --wwe2k14_rdr_crowd_ours and --wwe2k14_scene_records). Docs: docs/renderer/SKIP_GAME_WORK_MAP.md 13");

namespace wwe2k14::card_thin {

// kSkipBuilt lives in card_thin_core.h (flipped by the main session after the census of a milestone run, SKIP_GAME_WORK_MAP 13.7; until then mode 2 is mode 1).
inline constexpr uint32_t kCardOp15Return = 0x825130C8u;   // the return address of the card loop's op-15 recorder call (bl at 0x825130C4; scene_crowd.h kCardOp15Return)

inline Counters g_counters;
inline std::atomic<uint64_t> g_runs{0};
inline thread_local Decider t_dec;
inline std::atomic<uint64_t> g_logged_runs{0};

inline int RawMode() { return REXCVAR_GET(wwe2k14_rdr_crowd_card_thin); }
inline Mode EffectiveMode() {
  const int v = RawMode();
  if (v <= 0) return Mode::Off;
  if (v == 1) return Mode::Census;
  // review S6: lane 155's bake skip (mode 2) leaves a type with no stands block "unjoined" (its picture is joined by the bake's position), and every thin frame would be a hole. Until the cards
  // keep their join without the bake, the two cuts are exclusive: with the bake skip on, this one is census only.
  if (REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) >= 2) return Mode::Census;
  return kSkipBuilt ? Mode::Skip : Mode::Census;
}

inline void MaybeLog(Mode mode) {
  const uint64_t n = g_counters.runs.load(std::memory_order_relaxed);
  if (n == 0 || n % 600 != 1 || g_logged_runs.exchange(n) == n) return;
  const Counters& c = g_counters;
  const GateCounters& g = g_gate.c;
  REXLOG_INFO(
      "[rdr][crowd] card thin ({}): runs {}, proof runs {}, card loops {}, card draws seen {}, kept as first of a type {}, would drop {}, dropped {}, not proven {}; gate: full ok {}, full refused {}, thin ok {}, thin holes {}, "
      "resets {}, hole resets {}, latched {}",
      mode == Mode::Skip ? "skip" : "census, nothing touched", (unsigned long long)n, (unsigned long long)c.proof_runs.load(), (unsigned long long)c.loops.load(), (unsigned long long)c.card_calls.load(),
      (unsigned long long)c.kept_first.load(), (unsigned long long)c.would_drop.load(), (unsigned long long)c.dropped.load(), (unsigned long long)c.not_proven.load(), (unsigned long long)g.full_ok.load(),
      (unsigned long long)g.full_bad.load(), (unsigned long long)g.thin_ok.load(), (unsigned long long)g.thin_hole.load(), (unsigned long long)g.resets.load(), (unsigned long long)g.hole_resets.load(),
      g_gate.latched.load() ? "YES" : "no");
}

// The observers.
inline void OnCrowdDraw() {   // sub_82519508 entry: a new run of the crowd draw
  const Mode m = EffectiveMode();
  if (m == Mode::Off) return;
  t_dec.BeginRun(g_gate.policy, g_counters, g_runs, g_gate, NowMs());
  MaybeLog(m);
}
inline void OnCardLoop() {    // sub_82512F70 entry: the cards of one person type
  if (RawMode() > 0) t_dec.BeginTypeLoop(g_counters);
}
// The op-15 recorder hook (scene_crowd.h): true = do not record this draw. lr = the recorder's return address.
inline bool DropCard(uint32_t lr) {
  if (lr != kCardOp15Return) return false;
  const Mode m = EffectiveMode();
  if (m == Mode::Off) return false;
  const bool drop = t_dec.DropCardDraw(m, g_gate.policy, g_gate, g_counters, NowMs());
  g_counts.Note(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), drop);   // review B1: the positive signal and the count the binder checks the record against
  return drop;
}

}  // namespace wwe2k14::card_thin
