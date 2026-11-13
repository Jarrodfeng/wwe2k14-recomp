// Unit test of the far cards' thin record (Session 101, lane 175): the decision logic in src\scene_live\card_thin_core.h (the very file the game-side hook includes), driven the way the hook and the binder
// drive it. No game, no SDK, no GPU.
//   card_thin_test.exe               -> the unit checks of the shipped logic; exit 0 = all pass
//   card_thin_test.exe --plant N     -> the same checks against a deliberately wrong policy / driver N (a positive control): must exit 1
// Plants: drop_first, no_proof, stale_ignored, slack_ignored, no_latch, hole_ignored, skipped_as_hole, drop_all, proven_per_card, stamp_ignored.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/scene_live/card_thin_core.h"

namespace ct = wwe2k14::card_thin;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      ++g_fail;                                             \
      std::printf("FAIL line %d: %s\n", __LINE__, msg);     \
    }                                                       \
  } while (0)

static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }

struct World {
  ct::Policy policy;
  ct::Gate gate;
  ct::Counters counters;
  std::atomic<uint64_t> runs{0};
  ct::Decider dec;
  int64_t now = 1000;
  World() {
    if (Plant("drop_first")) policy.keep_first_of_type = false;
    if (Plant("no_proof")) policy.proof_runs = false;
    if (Plant("stale_ignored")) policy.honour_stale = false;
    if (Plant("slack_ignored")) policy.honour_thin_slack = false;
    if (Plant("no_latch")) policy.latch = false;
    if (Plant("hole_ignored")) policy.holes_reset = false;
    if (Plant("proven_per_card")) policy.snapshot_per_run = false;
    gate.policy = policy;
  }
  // One crowd draw run: `types` loops of `per_type` cards each. Returns {card draws recorded, card draws dropped}.
  struct Out { uint32_t recorded = 0, dropped = 0; bool proof = false; };
  Out Run(ct::Mode mode, uint32_t types, uint32_t per_type) {
    Out o;
    dec.BeginRun(policy, counters, runs, gate, now);
    o.proof = dec.proof_run;
    for (uint32_t t = 0; t < types; ++t) {
      dec.BeginTypeLoop(counters);
      for (uint32_t k = 0; k < per_type; ++k) {
        const bool drop = Plant("drop_all") ? (mode != ct::Mode::Off) : dec.DropCardDraw(mode, policy, gate, counters, now);
        ++(drop ? o.dropped : o.recorded);
      }
    }
    return o;
  }
  // The binder's side: the frame it binds is the run just recorded. thin = fewer card draws than the record's cards.
  bool wrong_offset = false;   // the stamp offset is wrong: the binder can never match a thin frame's tally (SF1)
  void Bind(const World::Out& o, uint32_t total, bool can_draw) {
    const bool thin = o.dropped != 0 && o.dropped + o.recorded == total;
    if (wrong_offset && thin) can_draw = false;
    // SF1: the refused thin frame (tally unknown, stream holds <= 20 card draws, record cards unpaired) is a HOLE; plant `refused_as_fullbad` reports it as a refused full frame
    ct::Kind k = !thin ? (can_draw ? ct::Kind::FullOk : ct::Kind::FullBad) : (can_draw ? ct::Kind::ThinOk : (Plant("refused_as_fullbad") ? ct::Kind::FullBad : ct::Kind::ThinHole));
    gate.Report(k, now);
  }
  void Warm(uint32_t frames = 4) {
    for (uint32_t i = 0; i < frames; ++i) {
      const Out o = Run(ct::Mode::Skip, 20, 5);
      Bind(o, 100, true);
      now += 16;
    }
  }
};

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) g_plant = argv[++i];

  // 1. Off: nothing counted, nothing dropped.
  {
    World w;
    const World::Out o = w.Run(ct::Mode::Off, 20, 5);
    CHECK(o.dropped == 0 && o.recorded == 100, "off: every card draw is recorded");
    CHECK(w.counters.card_calls.load() == 0, "off: the hook counts nothing");
  }
  // 2. Census: decides and counts, never drops (even proven).
  {
    World w;
    w.Warm(2);  // gate reports exist but the hook runs in census
    for (int i = 0; i < 4; ++i) w.gate.Report(ct::Kind::FullOk, w.now);
    w.runs.store(1);  // not a proof run
    const World::Out o = w.Run(ct::Mode::Census, 20, 5);
    CHECK(o.dropped == 0, "census: drops nothing");
    CHECK(o.recorded == 100, "census: all recorded");
    CHECK(w.counters.would_drop.load() == 80 && w.counters.switch_census.load() == 80, "census: counts what skip would have dropped (80 of 100)");
  }
  // 3. Skip: a cold gate drops nothing; after kNeedClean clean frames, every card but the first of each type's loop is dropped; a proof run records all.
  {
    World w;
    const World::Out cold = w.Run(ct::Mode::Skip, 20, 5);   // run 1 is a proof run anyway
    CHECK(cold.dropped == 0, "cold gate: nothing dropped");
    w.Warm(3);
    CHECK(w.gate.Proven(w.now), "three clean frames prove the gate");
    uint32_t dropped = 0, recorded = 0, proofs = 0;
    for (int i = 0; i < 70; ++i) {
      const World::Out o = w.Run(ct::Mode::Skip, 20, 5);
      w.Bind(o, 100, true);
      w.now += 16;
      if (o.proof) {
        ++proofs;
        CHECK(o.dropped == 0, "a proof run records every card");
      } else {
        CHECK(o.recorded == 20 && o.dropped == 80, "a thin run keeps exactly the first card of each of the 20 types");
      }
      dropped += o.dropped, recorded += o.recorded;
    }
    CHECK(proofs >= 2 && proofs <= 3, "a proof run about every 32 runs (70 runs: 2 or 3)");
    CHECK(dropped > 0, "the cut acts once proven");
    CHECK(w.gate.Proven(w.now), "ThinOk frames keep the gate proven");
  }
  // 4. The census of one proof cadence: runs 1, 33, 65, 97 are the proof runs.
  {
    World w;
    int proofs = 0;
    for (int i = 0; i < 100; ++i) proofs += w.Run(ct::Mode::Skip, 1, 1).proof;
    CHECK(proofs == 4, "100 runs: 4 proof runs (1, 33, 65, 97)");
    CHECK(w.counters.proof_runs.load() == 4, "the census counts them");
  }
  // 5. The binder's silence voids the proof (a menu, a stalled GPU, a skipped stretch): the next runs record in full.
  {
    World w;
    w.Warm(4);
    w.runs.store(1);
    w.now += 1000;  // 1 s without a report
    const World::Out o = w.Run(ct::Mode::Skip, 20, 5);
    CHECK(o.dropped == 0, "stale: nothing is dropped after the binder went silent for 1 s");
    // Review class "dropped frames count as failures": the sink skipped 100 frames (no reports at all). The silence must not latch the cut.
    for (int i = 0; i < 100; ++i) {
      if (Plant("skipped_as_hole")) w.gate.Report(ct::Kind::ThinHole, w.now);   // the plant: a skipped frame reported as a hole
      w.now += 16;
    }
    CHECK(w.gate.c.hole_resets.load() == 0, "frames the sink skipped are never reported, never a hole");
    CHECK(!w.gate.latched.load(), "a long gap does not latch the cut off");
    w.Warm(3);
    CHECK(w.gate.Proven(w.now), "after the gap three clean frames prove it again");
  }
  // 6. Thin frames without a full one (the sink skipped the proof frames): unproven after 2 K, a full frame proves it again at once.
  {
    World w;
    w.Warm(4);
    for (uint32_t i = 0; i < 2 * ct::kProofK + 1; ++i) {
      w.gate.Report(ct::Kind::ThinOk, w.now);
      w.now += 16;
    }
    CHECK(!w.gate.Proven(w.now), "more than 2 K thin frames bound with no full one: unproven");
    w.gate.Report(ct::Kind::FullOk, w.now);
    CHECK(w.gate.Proven(w.now), "one full frame bound proves it again");
  }
  // 7. A thin frame ours could not draw is a hole: the gate unproves at once, and 3 holes inside 10 s latch the cut off for the session.
  {
    World w;
    w.Warm(4);
    CHECK(w.gate.Proven(w.now), "proven before the hole");
    w.gate.Report(ct::Kind::ThinHole, w.now);
    CHECK(!w.gate.Proven(w.now), "a hole unproves the gate");
    CHECK(w.gate.c.hole_resets.load() == 1, "the hole is counted");
    for (int i = 0; i < 2; ++i) {
      w.now += 500;
      w.Warm(4);
      w.gate.Report(ct::Kind::ThinHole, w.now);
    }
    CHECK(w.gate.latched.load(), "3 holes in 10 s latch the cut off");
    w.Warm(10);
    CHECK(!w.gate.Proven(w.now), "latched for the session");
    w.runs.store(1);
    CHECK(w.Run(ct::Mode::Skip, 20, 5).dropped == 0, "latched: the game records everything");
  }
  // 8. A refused FULL frame resets without a hole and never latches.
  {
    World w;
    w.Warm(4);
    for (int i = 0; i < 6; ++i) {
      w.gate.Report(ct::Kind::FullBad, w.now);
      w.Warm(4);
    }
    CHECK(!w.gate.latched.load(), "full-frame refusals never latch (nothing is missing)");
    CHECK(w.gate.c.hole_resets.load() == 0, "and are not holes");
  }
  // 9. Two types that share one loop each keep their own first card (type_first is per loop, not per run).
  {
    World w;
    w.Warm(4);
    w.runs.store(1);
    const World::Out o = w.Run(ct::Mode::Skip, 3, 40);
    CHECK(o.recorded == 3 && o.dropped == 117, "3 types x 40 cards: 3 kept, 117 dropped");
  }

  // 10. Review S2: a binder report landing in the MIDDLE of a run does not change that run (all full or all thin, from its first card to its last).
  {
    World w;
    w.runs.store(1);
    w.dec.BeginRun(w.policy, w.counters, w.runs, w.gate, w.now);   // cold gate: the run is full
    uint32_t dropped = 0;
    for (int k = 0; k < 4; ++k) {
      w.dec.BeginTypeLoop(w.counters);
      for (int i = 0; i < 5; ++i) dropped += w.dec.DropCardDraw(ct::Mode::Skip, w.policy, w.gate, w.counters, w.now);
      if (k == 1) for (int j = 0; j < 4; ++j) w.gate.Report(ct::Kind::FullOk, w.now);   // the gate becomes proven mid-run
    }
    CHECK(dropped == 0, "a gate that becomes proven mid-run does not thin the rest of that run");
  }
  // 11. Review B1: the per-frame tally. Lookup needs the exact stamp; a frame the hook did not note is unknown; each op-15 call counts once (kept or dropped).
  {
    ct::CountRing ring;
    if (Plant("stamp_ignored")) ring.honour_stamp = false;
    for (int i = 0; i < 10; ++i) ring.Note(100, i != 0 && i % 3 != 0);
    uint32_t seen = 0, dropped = 0;
    uint64_t world = 0;
    uint32_t w1[12] = {1, 2, 3}, w2[12] = {1, 2, 4};
    ring.NoteWorld(100, w1), ring.NoteWorld(100, w2);
    CHECK(ring.Lookup(100, seen, dropped, world) && seen == 10 && dropped == 6, "the tally counts every card call and the dropped ones");
    CHECK(world == ct::CountRing::HashWords(w1) + ct::CountRing::HashWords(w2) && world != 0, "the world tally is the sum of the cards' hashes (order free)");
    CHECK(!ring.Lookup(101, seen, dropped, world), "a frame the hook did not note is unknown");
    ring.Note(164, false);   // the same slot, 64 frames later
    CHECK(!ring.Lookup(100, seen, dropped, world), "a slot reused 64 frames later no longer answers for the old frame");
  }

  // 12. SF1: a wrong stamp offset. Every thin frame is refused; as holes they latch the cut after 3 (10 s window), as refused full frames they would reset it forever (a flicker of missing cards).
  {
    World w;
    w.wrong_offset = true;
    uint32_t thin_runs = 0;
    for (int i = 0; i < 80; ++i) {
      const World::Out o = w.Run(ct::Mode::Skip, 20, 5);
      if (o.dropped) ++thin_runs;
      w.Bind(o, 100, true);
      w.now += 16;
    }
    CHECK(w.gate.latched.load(), "a wrong stamp offset latches the cut off after 3 holes");
    CHECK(thin_runs <= 3, "and no more than 3 thin runs ever happen");
  }

  std::printf(g_fail ? "card_thin_test: %d FAIL\n" : "card_thin_test: all pass\n", g_fail);
  return g_fail ? 1 : 0;
}
