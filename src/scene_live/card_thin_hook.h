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
//   =2  behaves as 1. SKIP is only --wwe2k14_rdr_card_cut (S102 T2, default off): the mode is taken ONCE per crowd draw run (OnCrowdDraw, Off included) and every observer of that run uses it (t_dec.mode).
// The binder side (crowd_cards_core.cpp's thin branch) acts only on a frame whose stream holds fewer card draws than the record has cards; a frame the game recorded in full is never touched. With the
// cut off no frame is thin, so nothing changes.
// Not for a determinism run (the records change: the det hash), not for MSAA / predicated tiling (a tiled frame runs the crowd draw once per tile: a run would be part proof, part thin).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../frame_stats.h"   // wwe2k14::g_guest_frame (the stamp of the per-frame tally)
#include "../renderer_adapter/rdr_switches.h"   // S102 T2 F2: the per-frame switch table (no game includes)
#include "card_thin_core.h"

REXCVAR_DECLARE(int32_t, wwe2k14_rdr_crowd_bake_skip);
REXCVAR_DECLARE(int32_t, wwe2k14_scene_crowd);   // S102 T2: the legacy runs path (scene_crowd.h)

REXCVAR_DEFINE_INT32(wwe2k14_rdr_crowd_card_thin, 0, "wwe2k14",
                     "Session 101 lane 175: the far cards' thin record. 0 off; 1 census (decide and count what would be dropped, touch nothing); 2 drop the op-15 draw of every far card except the first "
                     "of each person type's loop while the binder has proved ours draws the cards from the scene record (one run in 32 is recorded in full as the proof). 2 behaves as 1 (the cut itself is "
                     "--wwe2k14_rdr_card_cut). Needs --wwe2k14_rdr_crowd_cards (and so --wwe2k14_rdr_crowd_ours and --wwe2k14_scene_records). Docs: docs/renderer/SKIP_GAME_WORK_MAP.md 13");

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_card_cut, false, "wwe2k14",
                    "Session 102 lane T2: never send the far crowd cards the new renderer draws itself. On: the game does not record the op-15 draw of every far card except the first of each person type's loop, "
                    "while the binder has proved ours draws the cards from the scene record (the gate of card_thin_core.h: 3 clean frames, one run in 32 recorded in full as the proof, a hole unproves it, 3 holes "
                    "in 10 s latch it off). A thinned frame ours has no usable record for takes the last good record when the tally proves it holds this frame's cards, else it is a hole. Read once per crowd draw run (rdr_sw table, per-frame), so it can be flipped inside a route. Needs --wwe2k14_rdr_crowd_cards (so crowd_ours and "
                    "scene records); inert (census) with the legacy --wwe2k14_scene_crowd or --wwe2k14_rdr_crowd_bake_skip=2. Not for a determinism run. Claim: 2-4 ms of our render thread in Bryan's entrance. "
                    "Docs: docs/renderer/SKIP_GAME_WORK_MAP.md 13, 13.8");

REXCVAR_DEFINE_INT32(wwe2k14_rdr_record_skip, 0, "wwe2k14",
                     "Session 102 lane UQ1 (R22 cut 6): do not record what the new renderer never uses, at the source, on the game's update thread. A bit mask (PerFrame, read once per crowd draw run); default 0 = "
                     "byte for byte as before. Bit 1 = the crowd: the op-21 (the card's constants c76-c78) of every far card whose op-15 the card cut drops is not recorded either (the observer that feeds the scene record "
                     "reads the card's world before the recorder would run, so ours gets everything it read before; the first card of each type, a proof run, an unproven frame and a tiled frame record in full). "
                     "Needs --wwe2k14_rdr_card_cut doing its work (inert otherwise, counted as declined). Not for a determinism run. Claim: 0.4-0.55 ms of the update thread in Bryan's entrance (R22 profile share). "
                     "Bits 2 (stands members) and 4 (floor members) = the same for the stands / floor members whose op-15 the MEMBER CUT drops (member_thin_hook.h; need --wwe2k14_rdr_member_cut, inert otherwise). Bit 256 (0x100) = TEST ONLY, THE PICTURE IS WRONG (our renderer draws no crowd): our own observers on the update thread (crowd_live::OnOp21, OnPalettes) return at once and the game still records everything; one Surface flip measures what the observers cost. Bit 128 (0x80) = TEST ONLY, THE PICTURE IS WRONG: no crowd card, stands member or floor member record (op 21 + op 15 at 0x825130AC/C4, 0x82518554/74, 0x8276ED38/60) is made, whatever our renderer needs; it exists so one Surface flip measures the ceiling of removing all crowd recording from the update thread. "
                     "Docs: docs/renderer/RECORD_SKIP.md");

namespace wwe2k14::card_thin {

inline bool CutRequested() { return REXCVAR_GET(wwe2k14_rdr_card_cut); }

inline constexpr int64_t kRecordSkipCrowd = 1;   // S102 UQ1: wwe2k14_rdr_record_skip bit 1
inline constexpr int64_t kRecordSkipKnockOut = 0x80;   // TEST ONLY (the picture is wrong): every crowd card / stands member / floor member record
inline constexpr int64_t kRecordSkipObsKnockOut = 0x100;   // S102 OBS1 TEST ONLY: our observers (crowd_live::OnOp21 / OnPalettes) return at once; the game still records everything
inline std::atomic<bool> g_ko_obs{false};   // set once per crowd draw run (OnCrowdDraw)
inline bool ObserverKnockOut() { return g_ko_obs.load(std::memory_order_relaxed); }
inline std::atomic<bool> g_ko{false};   // set once per crowd draw run from the per-frame switch (OnCrowdDraw); read by the recorder hooks
// The recorder return addresses of the three sites (listings: bl 0x82BB27D8 / 0x82BB10C0 at 0x825130AC / C4, 0x82518554 / 74, 0x8276ED38 / 60; return = call + 4).
inline constexpr uint32_t kKoOp21[3] = {0x825130B0u, 0x82518558u, 0x8276ED3Cu};
inline constexpr uint32_t kKoOp15[3] = {0x825130C8u, 0x82518578u, 0x8276ED64u};
inline thread_local uint32_t t_ko_count = 0;

// S102 UQ1 census: per-thread plain counters (no atomic per card), flushed into the shared slots at the next crowd run's start and read by the log line. Names and strings are built at print time only.
inline thread_local Op21Tally t_op21_tally;
inline thread_local Tail t_tail;   // review F1: the last skipped card's world of the type loop in progress
struct alignas(64) Op21Shared {
  std::atomic<uint64_t> runs_armed{0}, taken{0}, forced{0}, tail{0}, ko{0}, declined[8]{};
};
inline Op21Shared g_op21;
inline std::atomic<uint64_t> g_op21_logged{0};
inline void FlushOp21Tally() {
  Op21Tally& t = t_op21_tally;
  if (t.taken) g_op21.taken.fetch_add(t.taken, std::memory_order_relaxed);
  if (t_ko_count) { g_op21.ko.fetch_add(t_ko_count, std::memory_order_relaxed); t_ko_count = 0; }
  if (t.forced) g_op21.forced.fetch_add(t.forced, std::memory_order_relaxed);
  if (t.tail) g_op21.tail.fetch_add(t.tail, std::memory_order_relaxed);
  for (int i = 0; i < 8; ++i)
    if (t.declined[i]) g_op21.declined[i].fetch_add(t.declined[i], std::memory_order_relaxed);
  t = Op21Tally{};
}
inline void MaybeLogOp21() {
  if (g_ko.load(std::memory_order_relaxed)) REXLOG_WARN("[rdr][record_skip] KNOCK-OUT (test only, the picture is wrong): crowd records not made so far {}", (unsigned long long)g_op21.ko.load(std::memory_order_relaxed));
  const uint64_t n = g_op21.runs_armed.load(std::memory_order_relaxed);
  static census_flush::Site cf_site;   // S102 CEN1: a flip phase end prints the census now (census_flush.h)
  if (n == 0 || (!census_flush::Due(cf_site) && (n % 600 != 1 || g_op21_logged.exchange(n) == n))) return;
  auto d = [](Decider::Op21 w) { return (unsigned long long)g_op21.declined[uint8_t(w)].load(std::memory_order_relaxed); };
  const unsigned long long dec = d(Decider::Op21::NotSkipMode) + d(Decider::Op21::FirstOfType) + d(Decider::Op21::ProofRun) + d(Decider::Op21::NotProven) + d(Decider::Op21::Tiled);
  REXLOG_INFO("[rdr][record_skip] crowd: frames {}, taken {}, declined {} (cut not skipping {}, first of a type {}, proof run {}, not proven {}, tiled {}), forced pairings {}, loop-exit op-21 records {}", (unsigned long long)n,
              (unsigned long long)g_op21.taken.load(std::memory_order_relaxed), dec, d(Decider::Op21::NotSkipMode), d(Decider::Op21::FirstOfType), d(Decider::Op21::ProofRun), d(Decider::Op21::NotProven),
              d(Decider::Op21::Tiled), (unsigned long long)g_op21.forced.load(std::memory_order_relaxed), (unsigned long long)g_op21.tail.load(std::memory_order_relaxed));
}

// kSkipBuilt lives in card_thin_core.h (flipped by the main session after the census of a milestone run, SKIP_GAME_WORK_MAP 13.7; until then mode 2 is mode 1).

inline Counters g_counters;
inline std::atomic<uint64_t> g_runs{0};
inline thread_local Decider t_dec;
inline bool Observing() { return t_dec.mode != Mode::Off; }   // S102 T2 F1: the run in progress on this thread is observed
inline std::atomic<uint64_t> g_logged_runs{0};

inline int RawMode() { return REXCVAR_GET(wwe2k14_rdr_crowd_card_thin); }
inline Mode EffectiveMode(bool cut) {   // S102 T2: called ONCE per run (OnCrowdDraw); F5: the exclusion cvars are read only when the cut is on
  if (!cut) return RawMode() <= 0 ? Mode::Off : Mode::Census;
  return ResolveMode(RawMode(), true, REXCVAR_GET(wwe2k14_scene_crowd), REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip));
}

inline void MaybeLog(Mode mode) {
  const uint64_t n = g_counters.runs.load(std::memory_order_relaxed);
  static census_flush::Site cf_site;   // S102 CEN1: a flip phase end prints the census now (census_flush.h)
  if (n == 0 || (!census_flush::Due(cf_site) && (n % 600 != 1 || g_logged_runs.exchange(n) == n))) return;
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
inline void ct_mask_publish(int64_t rs) { g_rs_mask.store(rs, std::memory_order_relaxed); }   // S102 UQ2: member_thin's OnCrowdDrawBegin (bits 2 / 4) reads it, after this
inline void OnCrowdDraw() {   // sub_82519508 entry: a new run of the crowd draw
  // S102 T2 F1 / F2: the switch is read here, ONCE per run, through the per-frame table (logs "[switch] seen"); the run's mode is stored in t_dec (Off included) and nothing else in the run reads a cvar.
  const bool cut = rdr_sw::BoolSeen(rdr_sw::kCardCut, CutRequested(), wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
  const Mode m = EffectiveMode(cut);
  if (BinderFlagCards(m)) g_counts.MarkSkip(wwe2k14::g_guest_frame.load(std::memory_order_relaxed));   // S102 T13 review F1 / F2: the binder's flag for THIS guest frame comes from the run's mode (Skip), never from the switch alone, never sticky
  t_dec.BeginRun(g_gate.policy, g_counters, g_runs, g_gate, NowMs(), m);
  // S102 UQ1: wwe2k14_rdr_record_skip bit 1 is read ONCE per run, here, through the per-frame table (logs "[switch] seen"), like the cut; the run is armed only while the cut is doing its work (Skip).
  FlushOp21Tally();
  const int64_t rs = rdr_sw::IntSeen(rdr_sw::kRecordSkip, REXCVAR_GET(wwe2k14_rdr_record_skip), wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
  t_dec.op21_armed = (rs & kRecordSkipCrowd) != 0;
  ct_mask_publish(rs);
  g_ko.store((rs & kRecordSkipKnockOut) != 0, std::memory_order_relaxed);
  g_ko_obs.store((rs & kRecordSkipObsKnockOut) != 0, std::memory_order_relaxed);   // S102 OBS1
  t_dec.op21_skipped = false;
  if (t_dec.op21_armed || g_ko.load(std::memory_order_relaxed)) {
    g_op21.runs_armed.fetch_add(1, std::memory_order_relaxed);
    MaybeLogOp21();
  }
  if (m != Mode::Off) MaybeLog(m);
}
inline void OnCardLoop() {    // sub_82512F70 entry: the cards of one person type
  t_tail.pending = false;   // S102 UQ1 F1
  if (t_dec.mode != Mode::Off) t_dec.BeginTypeLoop(g_counters);   // F1: the run's mode
}
// The op-15 recorder hook (scene_crowd.h): true = do not record this draw. lr = the recorder's return address.
inline thread_local bool t_replay = false;   // review F1: the op-15 step kept a card whose op-21 had been skipped: the hook replays the op-21 before the op-15 (TakeReplay)
inline bool TakeReplay() { const bool r = t_replay; t_replay = false; return r; }
inline bool DropCard(uint32_t lr, bool tiled = false) {
  bool replay = false;
  const bool drop = Op15Step(t_dec, g_gate.policy, g_gate, g_counters, g_counts, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), NowMs(), lr, tiled, t_op21_tally, &replay, &t_tail);
  if (replay) t_replay = true;   // the replayed op-21 IS recorded: nothing left to repair at the loop exit
  return drop;   // the pure body is in card_thin_core.h (unit-tested as shipped)
}

// S102 UQ1: the op-21 recorder hook's question (scene_crowd.h, AFTER crowd_live::OnOp21 has read the card's world from guest memory). true = do not record this card's op-21. `tiled` is a thunk so the command
// processor is asked only for a card that would otherwise be skipped.
template <class TiledFn, class ReadFn>
inline bool SkipCardOp21(uint32_t lr, uint32_t src, TiledFn tiled, ReadFn read32, uint32_t r4 = 76, uint32_t r6 = 3) {
  return Op21Step(t_dec, g_gate.policy, g_gate, NowMs(), lr, tiled, src, read32, t_tail, t_op21_tally, r4, r6);
}
// Review F1: at the exit of the card loop sub_82512F70, true = the last card's op-21 was skipped and `out` holds its 12 words: the hook records ONE op-21 with them (as scene_crowd.h Flush does for its runs), so the
// constant shadow of c76-c78 ends the loop as the game's own would. Cleared at every loop entry (OnCardLoop).
inline bool TakeTail(uint32_t* out) {
  if (!t_tail.pending) return false;
  t_tail.pending = false;
  std::memcpy(out, t_tail.w, sizeof(t_tail.w));
  ++t_op21_tally.tail;
  return true;
}

// TEST ONLY (bit 0x80): true = this crowd record (op 21 or op 15 at one of the three sites) is not made. Called by the two recorder hooks (scene_crowd.h) after the observers have read what they read.
inline bool KnockOut(uint32_t lr, bool op21) {
  if (!g_ko.load(std::memory_order_relaxed)) return false;
  const uint32_t* t = op21 ? kKoOp21 : kKoOp15;
  if (lr != t[0] && lr != t[1] && lr != t[2]) return false;
  ++t_ko_count;
  return true;
}

}  // namespace wwe2k14::card_thin
