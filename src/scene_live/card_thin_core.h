// The crowd's far cards, thinned at record time (Session 101, lane 175; docs\renderer\SKIP_GAME_WORK_MAP.md section 13). Decision logic only: no game, SDK or renderer includes. The game-side hook
// (card_thin_hook.h) asks it, the binder reports into it, and tools\crowd\card_thin_test.cpp runs this very file.
//
// What is cut: the card loop sub_82512F70 records, per card, an op-21 (c76-c78, the card's world) and an op-15 (the 4-index strip draw, call site 0x825130C4). While ours draws the cards from the scene
// record (lane 110 / CD, crowd_cards_core), the stream's card draws are only the trigger, the state source and a proof. THIN keeps the op-21 of EVERY card (the register file, the scene record's feed and
// the replay bookkeeping stay exactly the game's: no register is left stale, nothing needs a restore) and drops the op-15 of every card except the FIRST card of each person type's loop. The kept
// draw carries the type's picture (slot 0 = the type's bake copy), the frame's real state and its real constants, so the binder can draw the whole type from the record and check the kept card against
// the record's first card of the type, bit for bit (crowd_cards_core.cpp, the thin branch). A frame is thinned only while the gate is proven, and one run in kProofK is recorded in FULL (the proof
// frame: the binder re-verifies the whole pairing, the order, the pictures and the one-state rule on it).
//
//   --wwe2k14_rdr_crowd_card_thin=0 (default)  off. =1 CENSUS: the decision is made and counted, nothing is dropped. =2 SKIP: as built, behaves as 1 (card_thin_hook.h kSkipBuilt = false).
//
// Thread model: the hook runs on the game's recording thread(s) (Decider is thread_local in the hook), the binder reports from the render thread (Gate: relaxed atomics, one writer).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace wwe2k14::card_thin {

enum class Mode : uint8_t { Off, Census, Skip };

// The cut is inert until the main session flips this after reading a census (SKIP_GAME_WORK_MAP 13.7). In the core (not the hook) so the binder side (reader.h) can read it too: until then no
// frame is ever thin and the binder's thin branch is not even enabled (review S1).
inline constexpr bool kSkipBuilt = false;
inline constexpr uint32_t kHookToStreamOffset = 1;   // stream guest frame - the hook's guest frame stamp (the same measured constant as lane 152's d3::kHookToStreamOffset [C: J1]); [U] for the cards until a live census

inline constexpr uint32_t kProofK = 32;          // one crowd draw run in K is recorded in full
inline constexpr uint32_t kNeedClean = 3;        // consecutive clean bound frames before the game's record may be thinned
inline constexpr int64_t kStaleMs = 250;         // a binder report older than this counts as none (the binder went silent: a menu, a stalled GPU, a skipped stretch)
inline constexpr uint32_t kThinSlackK = 2;       // unproven when more than this many K thin frames were bound without a full one
inline constexpr uint32_t kLatchResets = 3;      // hole-type resets inside kLatchWindowMs latch the cut off for the session
inline constexpr int64_t kLatchWindowMs = 10000;

inline int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

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
struct Decider {
  uint64_t run = 0;           // this thread's crowd draw run index (global counter at BeginRun)
  bool proven_run = false;    // the gate's answer, taken ONCE at the run's start (review S2): a run is all full or thin from its first card to its last
  bool proof_run = true;      // this run is recorded in full
  bool type_first = false;    // the next card draw is the first of its type's loop

  void BeginRun(const Policy& p, Counters& c, std::atomic<uint64_t>& global_runs, const Gate& g, int64_t now_ms) {
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
  bool DropCardDraw(Mode mode, const Policy& p, Gate& g, Counters& c, int64_t now_ms) {
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
};

// The hook's per-guest-frame tally, the POSITIVE signal that a frame was thinned and its delivery check (review B1): how many card op-15 recorder calls the game made in the frame (every card, kept
// or dropped) and how many of them were dropped. Keyed by the guest frame the hook ran in (g_guest_frame). The binder looks up (stream frame - kHookToStreamOffset); a stamp that is not there is
// `unknown`. Each op-15 is one card, so `seen` must equal the record's card objects of that frame and `seen - dropped` the kept stream draws: the world match alone passes a record one frame off
// (card worlds are fixed per seat), the COUNT is what catches it.
struct CountRing {
  struct Slot {
    std::atomic<uint64_t> frame{~0ull};
    std::atomic<uint32_t> seen{0}, dropped{0};
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
      s.seen.store(0, std::memory_order_relaxed), s.dropped.store(0, std::memory_order_relaxed), s.world.store(0, std::memory_order_relaxed);
      s.frame.store(frame, std::memory_order_relaxed);
    }
    return s;
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

inline Gate g_gate;   // the one gate: the binder reports into it (binder.h), the hook asks it (card_thin_hook.h)
inline CountRing g_counts;   // the hook writes, the binder (crowd_ours via live.cpp) reads

}  // namespace wwe2k14::card_thin
