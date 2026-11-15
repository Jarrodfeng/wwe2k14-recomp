// The crowd's far cards, thinned at record time (Session 101, lane 175; docs\renderer\SKIP_GAME_WORK_MAP.md section 13). Decision logic only: no game, SDK or renderer includes. The game-side hook
// (card_thin_hook.h) asks it, the binder reports into it, and tools\crowd\card_thin_test.cpp runs this very file.
//
// What is cut: the card loop sub_82512F70 records, per card, an op-21 (c76-c78, the card's world) and an op-15 (the 4-index strip draw, call site 0x825130C4). While ours draws the cards from the scene
// record (lane 110 / CD, crowd_cards_core), the stream's card draws are only the trigger, the state source and a proof. THIN keeps the op-21 of EVERY card (as of the card cut alone; S102 UQ1's wwe2k14_rdr_record_skip bit 1 may skip them, see below: with it ON the shadow of c76-c78 is repaired by ONE op-21 at each type loop's exit) and drops the op-15 of every card except the FIRST card of each person type's loop. The kept
// draw carries the type's picture (slot 0 = the type's bake copy), the frame's real state and its real constants, so the binder can draw the whole type from the record and check the kept card against
// the record's first card of the type, bit for bit (crowd_cards_core.cpp, the thin branch). A frame is thinned only while the gate is proven, and one run in kProofK is recorded in FULL (the proof
// frame: the binder re-verifies the whole pairing, the order, the pictures and the one-state rule on it).
//
//   --wwe2k14_rdr_crowd_card_thin=0 (default)  off. =1 CENSUS: the decision is made and counted, nothing is dropped. =2 behaves as 1. SKIP (S102 T2) is only --wwe2k14_rdr_card_cut (card_thin_hook.h), default off.
//
// Thread model: the hook runs on the game's recording thread(s) (Decider is thread_local in the hook), the binder reports from the render thread (Gate: relaxed atomics, one writer).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace wwe2k14::card_thin {

enum class Mode : uint8_t { Off, Census, Skip };

// S102 T2: the build can skip. Whether it does is the per-run switch --wwe2k14_rdr_card_cut (default off), see ResolveMode. In the core (not the hook) so the binder side (reader.h) can read it too.
// S102 T2: true = the build can skip; WHETHER it does is the per-frame switch --wwe2k14_rdr_card_cut (card_thin_hook.h EffectiveMode), default off.
inline constexpr bool kSkipBuilt = true;
inline constexpr uint32_t kHookToStreamOffset = 1;   // stream guest frame - the hook's guest frame stamp (the same measured constant as lane 152's d3::kHookToStreamOffset [C: J1]); [U] for the cards until a live census

inline constexpr uint32_t kProofK = 32;          // one crowd draw run in K is recorded in full
inline constexpr uint32_t kNeedClean = 3;        // consecutive clean bound frames before the game's record may be thinned
inline constexpr int64_t kStaleMs = 250;         // a binder report older than this counts as none (the binder went silent: a menu, a stalled GPU, a skipped stretch)
inline constexpr uint32_t kThinSlackK = 2;       // unproven when more than this many K thin frames were bound without a full one
inline constexpr uint32_t kLatchResets = 3;      // hole-type resets inside kLatchWindowMs latch the cut off for the session
inline constexpr int64_t kLatchWindowMs = 10000;

inline int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// S102 T2: the hook's mode, from the switches it reads at every card draw (pure, so the unit test drives it). raw = --wwe2k14_rdr_crowd_card_thin (the old census switch), cut = --wwe2k14_rdr_card_cut
// (default off), scene_crowd = the legacy runs path, bake_skip = lane 155's switch. Only `cut` ever gives Skip; the old =2 stays census. `honour_exclusions=false` is the unit test's plant.
inline Mode ResolveMode(int raw, bool cut, int scene_crowd, int bake_skip, bool honour_exclusions = true) {
  if (!cut) return raw <= 0 ? Mode::Off : Mode::Census;
  if (honour_exclusions && scene_crowd != 0) return Mode::Census;    // SKIP_GAME_WORK_MAP 13.7 item 3: the legacy runs path and the cut are exclusive
  if (honour_exclusions && bake_skip >= 2) return Mode::Census;      // review S6: lane 155's bake skip leaves a type unjoined: every thin frame would be a hole
  return kSkipBuilt ? Mode::Skip : Mode::Census;
}

// Tunables; the shipped values are the defaults. The unit test's plants flip one at a time.
struct Policy {
  uint32_t proof_k = kProofK;
  uint32_t need_clean = kNeedClean;
  int64_t stale_ms = kStaleMs;
  uint32_t slack_k = kThinSlackK;
  bool keep_first_of_type = true;   // plant `drop_first`: the first card of a type is dropped too (no trigger, no picture, no state: a frame with no card draw at all)
  bool proof_runs = true;           // plant `no_proof`: no run is ever recorded in full
  bool honour_stale = true;         // plant `stale_ignored`: the binder's silence does not void the proof
  bool honour_thin_slack = true;    // plant `slack_ignored`: any number of thin frames without a full one stays proven
  bool latch = true;                // plant `no_latch`
  bool snapshot_per_run = true;     // plant `proven_per_card`: the gate is asked at every card draw instead of once at the run's start (a report landing mid-run makes the run part full, part thin)
  bool holes_reset = true;          // plant `hole_ignored`: a thin frame ours could not draw does not unprove the gate
};

// What the binder saw in one bound frame that has far cards.
enum class Kind : uint8_t {
  FullOk,    // the game recorded every card, ours drew them all (the full plan, every check)
  FullBad,   // a full frame the plan refused (nothing is missing: every stream card stays)
  ThinOk,    // a thin frame ours drew from the record, the kept cards checked against the record
  ThinHole,  // a thin frame ours could NOT draw: the dropped cards are gone and nothing replaces them (a hole; counted, required 0)
};

struct GateCounters {
  std::atomic<uint64_t> full_ok{0}, full_bad{0}, thin_ok{0}, thin_hole{0}, resets{0}, hole_resets{0}, void_stale{0}, void_slack{0}, latched_at_ms{0};
};

struct Gate {
  Policy policy;
  GateCounters c;
  std::atomic<uint32_t> clean{0};           // consecutive clean frames (any kind)
  std::atomic<uint32_t> thin_since_full{0};
  std::atomic<int64_t> last_ms{0};          // when the last report came (0 = never)
  std::atomic<bool> latched{false};
  std::atomic<uint32_t> recent_resets{0};   // hole-type resets inside the window
  std::atomic<int64_t> window_start_ms{0};

  void Report(Kind k, int64_t now_ms) {
    const bool was = Proven(now_ms);
    {   // lane 295 SF1: a proof does not survive silence (an arena load, a pause): the first report after > stale_ms starts the count again, it never re-proves from the previous scene's frames
      const int64_t t0 = last_ms.load(std::memory_order_relaxed);
      if (policy.honour_stale && t0 != 0 && now_ms - t0 > policy.stale_ms) clean.store(0, std::memory_order_relaxed), thin_since_full.store(0, std::memory_order_relaxed);
    }
    switch (k) {
      case Kind::FullOk:
        c.full_ok.fetch_add(1, std::memory_order_relaxed);
        thin_since_full.store(0, std::memory_order_relaxed);
        Clean();
        break;
      case Kind::ThinOk:
        c.thin_ok.fetch_add(1, std::memory_order_relaxed);
        thin_since_full.fetch_add(1, std::memory_order_relaxed);
        Clean();
        break;
      case Kind::FullBad:
        c.full_bad.fetch_add(1, std::memory_order_relaxed);
        Reset(was, false, now_ms);
        break;
      case Kind::ThinHole:
        c.thin_hole.fetch_add(1, std::memory_order_relaxed);
        if (policy.holes_reset) Reset(was, true, now_ms);
        break;
    }
    last_ms.store(now_ms, std::memory_order_relaxed);
  }

  // The hook asks, on a game thread, at every card draw it is about to record.
  bool Proven(int64_t now_ms) const {
    if (latched.load(std::memory_order_acquire)) return false;
    const int64_t t = last_ms.load(std::memory_order_relaxed);
    if (t == 0) return false;
    if (policy.honour_stale && now_ms - t > policy.stale_ms) return false;
    if (clean.load(std::memory_order_relaxed) < policy.need_clean) return false;
    if (policy.honour_thin_slack && thin_since_full.load(std::memory_order_relaxed) > policy.slack_k * policy.proof_k) return false;
    return true;
  }

 private:
  void Clean() {
    const uint32_t v = clean.load(std::memory_order_relaxed);
    if (v < 1000000u) clean.store(v + 1, std::memory_order_relaxed);
  }
  void Reset(bool was_proven, bool hole, int64_t now_ms) {
    if (was_proven) c.resets.fetch_add(1, std::memory_order_relaxed);
    clean.store(0, std::memory_order_relaxed);
    thin_since_full.store(0, std::memory_order_relaxed);
    if (!hole) return;
    c.hole_resets.fetch_add(1, std::memory_order_relaxed);
    const int64_t w = window_start_ms.load(std::memory_order_relaxed);
    if (w == 0 || now_ms - w > kLatchWindowMs) {
      window_start_ms.store(now_ms, std::memory_order_relaxed);
      recent_resets.store(1, std::memory_order_relaxed);
    } else if (recent_resets.fetch_add(1, std::memory_order_relaxed) + 1 >= kLatchResets && policy.latch) {
      c.latched_at_ms.store(uint64_t(now_ms), std::memory_order_relaxed);
      latched.store(true, std::memory_order_release);
    }
  }
};

// The game thread's census (relaxed atomics: read by the log line).
struct Counters {
  std::atomic<uint64_t> runs{0}, proof_runs{0}, loops{0}, card_calls{0}, kept_first{0}, would_drop{0}, dropped{0}, not_proven{0}, switch_census{0};
};

// One per recording thread: the run and the card loop in progress.
#ifdef RECORD_SKIP_PLANTS
inline int g_rs_plant = 0;   // test only (tools/crowd/record_skip_test.cpp): 1 hook_tiled_first, 2 tail_not_noted, 3 keep_forced_drop (a skipped op-21 whose op-15 the cut keeps is dropped anyway), 4 regs_ignored (cards), 11 member_pair_lost, 12 member_tail_not_noted, 13 member_regs_ignored, 14 member_keep_forced_drop
#define RS_PLANT(n) (::wwe2k14::card_thin::g_rs_plant == (n))
#else
#define RS_PLANT(n) false
#endif
#define RS_PLANT_KEEP_FORCED RS_PLANT(3)

struct Decider {
  uint64_t run = 0;           // this thread's crowd draw run index (global counter at BeginRun)
  bool proven_run = false;    // the gate's answer, taken ONCE at the run's start (review S2): a run is all full or thin from its first card to its last
  bool proof_run = true;      // this run is recorded in full
  bool type_first = false;    // the next card draw is the first of its type's loop
  Mode mode = Mode::Off;      // S102 T2 F1: the mode of THIS run, taken once at its start (Off included) and used by every observer until the next run; a switch flipped inside a run changes the next run only

  void BeginRun(const Policy& p, Counters& c, std::atomic<uint64_t>& global_runs, const Gate& g, int64_t now_ms, Mode run_mode) {
    mode = run_mode;
    type_first = false;
    proven_run = false;
    if (mode == Mode::Off) return;
    proven_run = g.Proven(now_ms);
    run = global_runs.fetch_add(1, std::memory_order_relaxed) + 1;
    proof_run = !p.proof_runs ? false : (run % p.proof_k == 1);   // the 1st, 33rd, ... run: a proof run right at the start (also after a latch-free restart)
    c.runs.fetch_add(1, std::memory_order_relaxed);
    if (proof_run) c.proof_runs.fetch_add(1, std::memory_order_relaxed);
    type_first = false;
  }
  void BeginTypeLoop(Counters& c) {
    type_first = true;
    c.loops.fetch_add(1, std::memory_order_relaxed);
  }
  // True = do not record this card's op-15. The op-21 is never touched.
  bool DropCardDraw(const Policy& p, Gate& g, Counters& c, int64_t now_ms) {   // S102 T2 F1: the run's mode (`mode`), never a per-call one
    if (mode == Mode::Off) return false;
    c.card_calls.fetch_add(1, std::memory_order_relaxed);
    const bool first = type_first && p.keep_first_of_type;
    type_first = false;
    if (first) {
      c.kept_first.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (proof_run) return false;
    if (!(p.snapshot_per_run ? proven_run : g.Proven(now_ms))) {
      c.not_proven.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    c.would_drop.fetch_add(1, std::memory_order_relaxed);
    if (mode != Mode::Skip) {
      c.switch_census.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    c.dropped.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // ---- S102 UQ1 (wwe2k14_rdr_record_skip bit 1, docs\renderer\RECORD_SKIP.md): the card's op-21 goes with its op-15 ----
  // The card loop records, per card, op 21 (bl 0x825130AC) and, straight-line after it (listing 0x825130B0-0x825130C4: four `li`, one `addi`, no branch), op 15 (bl 0x825130C4). A card whose op-15 this
  // decider drops needs no constants recorded for it: no draw will read them. Rule: the op-21 is skipped ONLY when the same card's op-15 is certain to be dropped, and the op-15 then follows
  // the op-21's decision (op21_skipped), whatever the gate or the tiling flag say in between. Everything the decision reads is per-run state fixed at BeginRun / BeginTypeLoop (proven_run is the gate's answer taken ONCE at the run's start),
  // so the peek cannot disagree with the op-15's own decision in a real run; the pairing flag makes it hold in a run that is not (the unit test plants a gate that flips between the two calls).
  bool op21_armed = false;     // this run: wwe2k14_rdr_record_skip bit 1 was set when the run began (BeginRun's caller, once per run)
  bool op21_skipped = false;   // the op-21 of the card in progress was NOT recorded: its op-15 MUST be dropped
  enum class Op21 : uint8_t { Off, NotSkipMode, FirstOfType, ProofRun, NotProven, Tiled, Taken };
  // Const: no counter, no state change. Mirrors DropCardDraw's drop conditions one for one (the test asserts equal answers over every state combination).
  Op21 PeekOp21(const Policy& p, const Gate& g, int64_t now_ms) const {
    if (!op21_armed) return Op21::Off;
    if (mode != Mode::Skip) return Op21::NotSkipMode;
    if (type_first && p.keep_first_of_type) return Op21::FirstOfType;
    if (proof_run) return Op21::ProofRun;
    if (!(p.snapshot_per_run ? proven_run : g.Proven(now_ms))) return Op21::NotProven;
    return Op21::Taken;
  }
  // The op-21 hook's question (after the observer has read the card's world). true = do not record this op-21. `tiled` = MSAA / predicated tiling: every card stays recorded.
  bool SkipCardOp21(const Policy& p, const Gate& g, int64_t now_ms, bool tiled, Op21* why = nullptr) {
    Op21 w = PeekOp21(p, g, now_ms);
    if (w == Op21::Taken && tiled) w = Op21::Tiled;
    if (why) *why = w;
    op21_skipped = (w == Op21::Taken);
    return op21_skipped;
  }
  // The op-15 hook's decision for a card whose op-15 is about to be recorded or dropped: `drop` is DropCardDraw's answer (false when the hook did not ask or the frame is tiled). A card whose op-21 was skipped and whose
  // op-15 the cut now KEEPS (the gate latched, or tiling came on, between the two calls; review F1) must not lose its constants: *replay is set and the hook records the op-21 (from the matrix still on the
  // loop's stack) before the op-15; the card keeps BOTH. Otherwise the pairing holds (dropped op-21, dropped op-15). The flag is consumed here.
  bool PairOp15(bool drop, bool* replay = nullptr) {
    const bool skipped = op21_skipped;
    op21_skipped = false;
    if (skipped && !drop) {
      if (replay) *replay = true;
      return RS_PLANT_KEEP_FORCED ? true : false;
    }
    return drop;
  }
};

// The hook's per-guest-frame tally, the POSITIVE signal that a frame was thinned and its delivery check (review B1): how many card op-15 recorder calls the game made in the frame (every card, kept
// or dropped) and how many of them were dropped. Keyed by the guest frame the hook ran in (g_guest_frame). The binder looks up (stream frame - kHookToStreamOffset); a stamp that is not there is
// `unknown`. Each op-15 is one card, so `seen` must equal the record's card objects of that frame and `seen - dropped` the kept stream draws: the world match alone passes a record one frame off
// (card worlds are fixed per seat), the COUNT is what catches it.
struct CountRing {
  struct Slot {
    std::atomic<uint64_t> frame{~0ull};
    std::atomic<uint32_t> seen{0}, dropped{0};
    std::atomic<uint8_t> skip{0};     // S102 T13 review F1: the hook's run mode for this frame was Skip (the binder's per-frame flag; never sticky)
    std::atomic<uint64_t> world{0};   // the SUM of FNV-1a(12 words of c76-c78) over every card op-21 of the frame (order free): which cards, not only how many
  };
  Slot slots[64];
  bool honour_stamp = true;   // plant `stamp_ignored`: a lookup returns whatever sits in the slot
  static uint64_t HashWords(const uint32_t* w) {   // the same FNV-1a over the 12 words crowd_cards_core.cpp uses for its pairing
    uint64_t h = 0xcbf29ce484222325ull;
    for (int k = 0; k < 12; ++k) h = (h ^ w[k]) * 0x100000001b3ull;
    return h;
  }
  Slot& For(uint64_t frame) {
    Slot& s = slots[frame & 63];
    if (s.frame.load(std::memory_order_relaxed) != frame) {
      s.seen.store(0, std::memory_order_relaxed), s.dropped.store(0, std::memory_order_relaxed), s.world.store(0, std::memory_order_relaxed), s.skip.store(0, std::memory_order_relaxed);
      s.frame.store(frame, std::memory_order_relaxed);
    }
    return s;
  }
  void MarkSkip(uint64_t frame) { For(frame).skip.store(1, std::memory_order_relaxed); }   // OnCrowdDraw, only for a run whose mode is Skip
  bool SkipAt(uint64_t frame) const {   // the render thread
    const Slot& s = slots[frame & 63];
    return s.frame.load(std::memory_order_relaxed) == frame && s.skip.load(std::memory_order_relaxed) != 0;
  }
  void Note(uint64_t frame, bool dropped) {   // the recording thread, once per card op-15 call
    Slot& s = For(frame);
    s.seen.fetch_add(1, std::memory_order_relaxed);
    if (dropped) s.dropped.fetch_add(1, std::memory_order_relaxed);
  }
  void NoteWorld(uint64_t frame, const uint32_t words[12]) {   // the recording thread, once per card op-21 (the world the card is recorded with)
    For(frame).world.fetch_add(HashWords(words), std::memory_order_relaxed);
  }
  bool Lookup(uint64_t frame, uint32_t& seen, uint32_t& dropped, uint64_t& world) const {   // the render thread
    const Slot& s = slots[frame & 63];
    if (honour_stamp && s.frame.load(std::memory_order_relaxed) != frame) return false;
    seen = s.seen.load(std::memory_order_relaxed), dropped = s.dropped.load(std::memory_order_relaxed), world = s.world.load(std::memory_order_relaxed);
    return true;
  }
};

inline constexpr bool BinderFlagCards(Mode m) { return m == Mode::Skip; }   // S102 T13 review F1 / F2: the binder's per-frame flag is the run's MODE (Skip), carried in the CountRing slot; the sticky the old sticky flag is gone
inline Gate g_gate;   // the one gate: the binder reports into it (binder.h), the hook asks it (card_thin_hook.h)
inline CountRing g_counts;   // the hook writes, the binder (crowd_ours via live.cpp) reads

// ---- S102 UQ1 (wwe2k14_rdr_record_skip bit 1): the two hook bodies as PURE functions, so the unit test drives the shipped code and not a hand-made mirror (review F2) ----
inline constexpr uint32_t kCardOp21Return = 0x825130B0u;   // the return address of the card loop's op-21 recorder call (bl at 0x825130AC; scene_crowd.h kCardOp21Return)
inline constexpr uint32_t kCardOp15Return = 0x825130C8u;   // the return address of the card loop's op-15 recorder call (bl at 0x825130C4; scene_crowd.h kCardOp15Return)

// The wwe2k14_rdr_record_skip mask of the crowd draw run in progress, read once per run by card_thin's OnCrowdDraw (the first crowd observer) and used by member_thin's (bit 2 stands, bit 4 floor).
inline std::atomic<int64_t> g_rs_mask{0};


// The per-thread census of the skip (plain counters; flushed by the hook at the next run's start).
struct Op21Tally {
  uint32_t taken = 0, declined[8] = {};   // declined by Decider::Op21 reason (index = enum value)
  uint32_t ko = 0;                        // TEST ONLY knock-out: records not made
  uint32_t forced = 0, tail = 0;          // replayed op-21s (a skipped op-21 whose op-15 the cut then kept: rare, counted); loop-exit op-21 records made
};

// The last SKIPPED card's world of the loop in progress (review F1): the game's op-21 leaves c76-c78 = the last card's world in the replay's constant shadow; with the tail skipped the shadow would keep the last KEPT
// card's, so the loop's exit records ONE op-21 with this (as the legacy scene_crowd Flush does for its runs). pending = the last card of the loop in progress had its op-21 skipped.
struct Tail {
  bool pending = false;
  uint32_t w[12] = {};
};

// The op-21 recorder hook's step, AFTER the observer has read the card's world. true = do not record. read32(addr) reads a guest word (the 12 words at src = the recorder's r5).
template <class TiledFn, class ReadFn>
inline bool Op21Step(Decider& d, const Policy& p, const Gate& g, int64_t now_ms, uint32_t lr, TiledFn tiled, uint32_t src, ReadFn read32, Tail& tail, Op21Tally& tally, uint32_t r4 = 76, uint32_t r6 = 3) {
  if (lr != kCardOp21Return || !d.op21_armed) return false;
  if ((r4 != 76 || r6 != 3) && !RS_PLANT(4)) {   // review F2: the replayed / repaired op-21 is always (76, 3); any other call stays recorded
    ++tally.declined[uint8_t(Decider::Op21::Tiled)];
    tail.pending = false;
    d.op21_skipped = false;
    return false;
  }
  Decider::Op21 w = d.PeekOp21(p, g, now_ms);
  const bool t = (w == Decider::Op21::Taken) && tiled();
  const bool skip = d.SkipCardOp21(p, g, now_ms, t, &w);
  if (skip) {
    ++tally.taken;
    tail.pending = true;
    if (!RS_PLANT(2))
      for (uint32_t k = 0; k < 12; ++k) tail.w[k] = read32(src + 4 * k);
  } else {
    ++tally.declined[uint8_t(w)];
    tail.pending = false;   // this card's op-21 IS recorded: the shadow already holds its world
  }
  return skip;
}

// The op-15 recorder hook's step (the former card_thin_hook.h DropCard body). true = do not record this card's draw. Counts the card in the per-frame tally.
inline bool Op15Step(Decider& d, const Policy& p, Gate& g, Counters& c, CountRing& counts, uint64_t guest_frame, int64_t now_ms, uint32_t lr, bool tiled, Op21Tally& tally, bool* replay = nullptr, Tail* tail = nullptr) {
  if (lr != kCardOp15Return) return false;
  if (d.mode == Mode::Off) return false;   // F1: the run's mode, taken at OnCrowdDraw
  if (d.op21_skipped && !RS_PLANT(1)) {   // this card's op-21 was NOT recorded: the pair must stay whole whatever the gate or the tiling flag say now. DropCardDraw still runs (it advances the type loop and the counters).
    bool dd = d.DropCardDraw(p, g, c, now_ms);
    if (tiled) dd = false;   // tiling came on after the op-21 was skipped: the card is kept (the cut's own rule), so its op-21 is replayed
    bool rp = false;
    const bool drop = d.PairOp15(dd, &rp);
    if (rp) {
      ++tally.forced;
      if (replay) *replay = true;
      if (tail) tail->pending = false;   // the replayed op-21 IS recorded: nothing to repair at the loop exit
    }
    counts.Note(guest_frame, drop);
    return drop;
  }
  if (tiled) {   // S102 T2: tiling: the run is not droppable; the tally still counts the card (seen, not dropped)
    counts.Note(guest_frame, false);
    if (d.op21_skipped) d.op21_skipped = false;   // plant hook_tiled_first only: the pairing was bypassed
    return false;
  }
  const bool drop = d.DropCardDraw(p, g, c, now_ms);
  counts.Note(guest_frame, drop);   // review B1: the positive signal and the count the binder checks the record against
  return drop;
}

}  // namespace wwe2k14::card_thin
