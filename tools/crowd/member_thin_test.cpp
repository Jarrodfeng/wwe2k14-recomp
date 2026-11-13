// Unit test of the crowd members' thin record (Session 101, lane 272): the decision logic in src\scene_live\member_thin_core.h (the very file the game-side hook includes), driven the way the hook and
// the binder drive it, per call site, ONE OP 15 AT A TIME (the RL1 rule, SKIP_GAME_WORK_MAP 16.9 / 20.7). No game, no SDK, no GPU.
//   member_thin_test.exe               -> the unit checks of the shipped logic; exit 0 = all pass
//   member_thin_test.exe --plant N     -> the same checks against a deliberately wrong policy / driver N (a positive control): must exit 1
// Plants: run_per_frame, calls_collapsed, drop_first, no_palette_dropped, tiled_double, stale_ignored, slack_ignored, no_latch, hole_ignored, skipped_as_hole, proven_per_card, no_proof, stamp_ignored,
//         weak_check (offset), floor_site_dropped_unobserved, drop_all, refused_as_fullbad, repeated_run_boundary.
// Lane 295 (the skip arm, replayed): stamp_offset0 (the tally is taken from stream frame - 0), elsewhere_ignored, miss_ignored, ambiguous_counts, stamp_unproven_dropped, verify_blind.
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/scene_live/member_thin_core.h"

namespace mt = wwe2k14::member_thin;
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

// One crowd draw run: a list of member runs per site; each has `members` op-15 calls.
struct RunShape { int site; uint32_t members; bool palette = true; bool tiled = false; };

struct World {
  ct::Policy policy;
  ct::Gate gate;
  mt::Counters counters;
  std::atomic<uint64_t> runs{0};
  mt::Decider dec;
  mt::CountRing ring;
  mt::StampProof stamp;      // lane 295
  mt::JudgePlants jp;        // lane 295
  bool use_stamp = false;    // lane 295: the replay asks the stamp proof at every run start (the older tests leave it out: it answers true)
  bool moving = false;       // lane 295: the crowd's worlds change every frame (a static crowd matches every offset)
  int64_t now = 1000;
  uint64_t frame = 100;
  World() {
    if (Plant("drop_first")) policy.keep_first_of_type = false;
    if (Plant("no_proof")) policy.proof_runs = false;
    if (Plant("stale_ignored")) policy.honour_stale = false;
    if (Plant("slack_ignored")) policy.honour_thin_slack = false;
    if (Plant("no_latch")) policy.latch = false;
    if (Plant("hole_ignored")) policy.holes_reset = false;
    if (Plant("proven_per_card")) policy.snapshot_per_run = false;
    gate.policy = policy;
    if (Plant("no_palette_dropped")) dec.honour_palette = false;
    if (Plant("tiled_double")) dec.honour_tiled = false;
    if (Plant("run_per_frame")) dec.boundary_per_run = false;
    if (Plant("stamp_ignored")) ring.honour_stamp = false;
    if (Plant("miss_ignored")) stamp.honour_miss = false;
    if (Plant("ambiguous_counts")) stamp.count_ambiguous = true;
    if (Plant("stamp_unproven_dropped")) dec.honour_stamp_run = false;
    if (Plant("stamp_offset0")) jp.offset0 = true;
    if (Plant("elsewhere_ignored")) jp.skip_elsewhere = true;
    if (Plant("judge_count_only")) jp.count_only = true;
    if (Plant("stale_streak_kept")) stamp.honour_stale = false;
  }
  struct Out { uint32_t recorded[mt::kSites] = {0, 0}, dropped[mt::kSites] = {0, 0}, runs[mt::kSites] = {0, 0}, calls[mt::kSites] = {0, 0}; uint64_t world[mt::kSites] = {0, 0}; bool proof = false; uint64_t frame = 0; };
  // Records one crowd draw: for each member run, the site's op 14 then `members` x (op 21, op 15). Worlds are made up from (site, run, member).
  Out Run(mt::Mode mode, const std::vector<RunShape>& shape) {
    Out o;
    o.frame = frame;
    dec.BeginRun(policy, counters, runs, gate, now, use_stamp ? stamp.Proven(now) : true);
    o.proof = dec.proof_run;
    uint32_t ri = 0;
    for (const RunShape& r : shape) {
      dec.BeginSiteRun(r.site, counters);
      ++o.runs[r.site];
      bool collapsed_noted = false;
      for (uint32_t m = 0; m < r.members; ++m) {
        uint32_t w[12] = {uint32_t(r.site), ri, m, 7u + (moving ? uint32_t(frame) * 2654435761u : 0u)};
        const bool observe = !(Plant("floor_site_dropped_unobserved") && r.site == mt::kFloor);
        const uint64_t h = mt::CountRing::HashWords(w);
        o.world[r.site] += h;                                    // the record's (the game's) member
        if (observe) ring.NoteWorld(frame, r.site, w);            // the op 21 the hook saw
        if (Plant("repeated_run_boundary")) dec.BeginSiteRun(r.site, counters);   // the plant: an op 14 per member (the boundary never closes: every member is a first)
        mt::Query q;
        q.palette_ok = r.palette, q.tiled = r.tiled;
        const bool drop = Plant("drop_all") ? (mode != mt::Mode::Off) : dec.DropMemberDraw(r.site, mode, policy, gate, counters, now, q);
        if (Plant("calls_collapsed")) {   // the plant: the tally counts a run as ONE call (the RL1 diagnosis: runs, not calls)
          if (!collapsed_noted) ring.Note(frame, r.site, false), collapsed_noted = true;
        } else {
          ring.Note(frame, r.site, drop);
        }
        ++o.calls[r.site];
        ++(drop ? o.dropped[r.site] : o.recorded[r.site]);
      }
      ++ri;
    }
    frame++;
    return o;
  }
  // The binder's side for one site: the record holds `members` and the world sum the ring was told; `stream_draws` kept draws are in the stream.
  mt::Verdict Check(const Out& o, int site, uint64_t record_world_override = ~0ull, uint64_t record_members_override = ~0ull) {
    mt::Tally t;
    t.known = ring.Lookup(o.frame, site, t.seen, t.dropped, t.world);
    const uint64_t rm = record_members_override != ~0ull ? record_members_override : o.calls[site];
    uint64_t rw = record_world_override != ~0ull ? record_world_override : o.world[site];
    if (Plant("weak_check")) {   // the plant: a binder that tests only the count (a record one frame off with the same member count passes)
      t.world = rw;
    }
    return mt::CheckTally(t, rm, rw, o.recorded[site]);
  }
  bool wrong_offset = false;
  void Bind(const Out& o, bool can_draw) {
    const bool thin = o.dropped[0] + o.dropped[1] != 0;
    ct::Kind k = !thin ? (can_draw ? ct::Kind::FullOk : ct::Kind::FullBad) : (can_draw ? ct::Kind::ThinOk : (Plant("refused_as_fullbad") ? ct::Kind::FullBad : ct::Kind::ThinHole));
    gate.Report(k, now);
  }
  static std::vector<RunShape> Shape(uint32_t stands_runs = 30, uint32_t stands_members = 5, uint32_t floor_runs = 20, uint32_t floor_members = 4) {
    std::vector<RunShape> s;
    for (uint32_t i = 0; i < stands_runs; ++i) s.push_back({mt::kStands, stands_members});
    for (uint32_t i = 0; i < floor_runs; ++i) s.push_back({mt::kFloor, floor_members});
    return s;
  }
  // Lane 295: the binder's side for the stream frame (hook frame + skew): the tallies at stream - 0 / 1 / 2, the Judge, then the reports the binder makes (the gate, the stamp proof).
  struct Bound { mt::Judgement j; mt::VerifyArm va; };
  Bound BindStream(const Out& o, uint64_t skew = 1, uint64_t record_world_bump = 0) {
    Bound b;
    mt::FrameView fv;
    const uint64_t stream_frame = o.frame + skew;
    for (int k = 0; k < 3; ++k)
      for (int s = 0; s < mt::kSites; ++s) fv.at[k][s].known = ring.Lookup(stream_frame - uint64_t(k), s, fv.at[k][s].seen, fv.at[k][s].dropped, fv.at[k][s].world);
    for (int s = 0; s < mt::kSites; ++s) fv.rec_members[s] = o.calls[s], fv.rec_world[s] = o.world[s] + record_world_bump, fv.stream_draws[s] = o.recorded[s];
    b.j = mt::Judge(fv, jp);
    ct::Kind k = b.j.take == mt::Take::Thin ? ct::Kind::ThinOk : b.j.take == mt::Take::Hole ? ct::Kind::ThinHole : (b.j.ev == mt::StampEv::Miss ? ct::Kind::FullBad : ct::Kind::FullOk);
    gate.Report(k, now);
    stamp.Report(b.j.ev, now);
    if (b.j.take == mt::Take::Full && !Plant("verify_blind"))
      for (int s = 0; s < mt::kSites; ++s) b.va.Calls(fv.at[1][s], fv.stream_draws[s], fv.rec_world[s]);
    return b;
  }
  void Warm(uint32_t frames = 4) {
    for (uint32_t i = 0; i < frames; ++i) {
      const Out o = Run(mt::Mode::Skip, Shape());
      Bind(o, true);
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
    const World::Out o = w.Run(mt::Mode::Off, World::Shape());
    CHECK(o.dropped[0] + o.dropped[1] == 0 && o.recorded[0] == 150 && o.recorded[1] == 80, "off: every member op 15 is recorded");
    CHECK(w.counters.calls[0].load() == 0 && w.counters.calls[1].load() == 0, "off: the hook counts nothing");
  }
  // 2. Census: decides and counts per site, never drops (even proven).
  {
    World w;
    for (int i = 0; i < 4; ++i) w.gate.Report(ct::Kind::FullOk, w.now);
    w.runs.store(1);   // not a proof run
    const World::Out o = w.Run(mt::Mode::Census, World::Shape());
    CHECK(o.dropped[0] + o.dropped[1] == 0, "census: drops nothing");
    CHECK(w.counters.would_drop[0].load() == 120 && w.counters.would_drop[1].load() == 60, "census: counts what skip would have dropped (stands 150-30, floor 80-20)");
    CHECK(w.counters.switch_census[0].load() == 120, "census: the switch census equals would drop");
  }
  // 3. Skip: a cold gate drops nothing; once proven, every member but the first of each run is dropped, per site; a proof run records all.
  {
    World w;
    const World::Out cold = w.Run(mt::Mode::Skip, World::Shape());   // run 1 is a proof run anyway
    CHECK(cold.dropped[0] + cold.dropped[1] == 0, "cold gate: nothing dropped");
    w.Warm(3);
    CHECK(w.gate.Proven(w.now), "three clean frames prove the gate");
    uint32_t proofs = 0;
    uint64_t dropped = 0;
    for (int i = 0; i < 70; ++i) {
      const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
      w.Bind(o, true);
      w.now += 16;
      if (o.proof) {
        ++proofs;
        CHECK(o.dropped[0] + o.dropped[1] == 0, "a proof run records every member");
      } else {
        CHECK(o.recorded[0] == 30 && o.dropped[0] == 120, "a thin run keeps exactly the first member of each of the 30 stands runs");
        CHECK(o.recorded[1] == 20 && o.dropped[1] == 60, "and of each of the 20 floor runs");
      }
      dropped += o.dropped[0] + o.dropped[1];
    }
    CHECK(proofs >= 2 && proofs <= 3, "a proof run about every 32 runs (70 runs: 2 or 3)");
    CHECK(dropped > 0, "the cut acts once proven");
    CHECK(w.gate.Proven(w.now), "ThinOk frames keep the gate proven");
  }
  // 4. 100 runs: 4 proof runs (1, 33, 65, 97).
  {
    World w;
    int proofs = 0;
    for (int i = 0; i < 100; ++i) proofs += w.Run(mt::Mode::Skip, {{mt::kStands, 1}}).proof;
    CHECK(proofs == 4, "100 runs: 4 proof runs (1, 33, 65, 97)");
    CHECK(w.counters.proof_runs.load() == 4, "the census counts them");
  }
  // 5. Silence voids the proof; a gap does not latch.
  {
    World w;
    w.Warm(4);
    w.runs.store(1);
    w.now += 1000;
    const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
    CHECK(o.dropped[0] + o.dropped[1] == 0, "stale: nothing is dropped after the binder went silent for 1 s");
    for (int i = 0; i < 100; ++i) {
      if (Plant("skipped_as_hole")) w.gate.Report(ct::Kind::ThinHole, w.now);
      w.now += 16;
    }
    CHECK(w.gate.c.hole_resets.load() == 0, "frames the sink skipped are never reported, never a hole");
    CHECK(!w.gate.latched.load(), "a long gap does not latch the cut off");
    w.Warm(3);
    CHECK(w.gate.Proven(w.now), "after the gap three clean frames prove it again");
  }
  // 6. Thin frames without a full one: unproven after 2 K.
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
  // 7. A thin hole unproves; 3 holes in 10 s latch.
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
    const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
    CHECK(o.dropped[0] + o.dropped[1] == 0, "latched: the game records everything");
    CHECK(w.counters.kept_why[0][mt::kLatched].load() > 0, "and the census names the reason");
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
  // 9. RL1: the tally counts CALLS (one op 15 each), never runs. seen != runs on every frame; seen == the record's members; kept == runs.
  {
    World w;
    w.Warm(4);
    w.runs.store(1);
    const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
    CHECK(o.runs[0] == 30 && o.calls[0] == 150, "the shape has more calls than runs");
    for (int s = 0; s < mt::kSites; ++s) {
      mt::Tally t;
      t.known = w.ring.Lookup(o.frame, s, t.seen, t.dropped, t.world);
      CHECK(t.known && t.seen != o.runs[s], "RL1: seen != runs");
      CHECK(t.seen == o.calls[s], "seen == the op-15 calls of the site");
      CHECK(t.seen - t.dropped == o.runs[s], "kept == runs (one kept draw per run)");
      CHECK(w.Check(o, s) == mt::Verdict::Ok, "an honest record and stream pass the delivery check");
    }
  }
  // 10. The delivery check refuses a record of another frame: other count, other members at the same count, a stream with a missing kept draw, an unknown frame.
  {
    World w;
    w.Warm(4);
    w.runs.store(1);
    const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
    CHECK(w.Check(o, 0, ~0ull, o.calls[0] + 1) == mt::Verdict::SeenMismatch, "a record with one member more is refused (count)");
    CHECK(w.Check(o, 0, o.world[0] + 1, ~0ull) == mt::Verdict::WorldMismatch, "a record with the same count and other members is refused (world sum): the +1 offset with an equal-size crowd");
    mt::Tally t;
    t.known = w.ring.Lookup(o.frame, 0, t.seen, t.dropped, t.world);
    CHECK(mt::CheckTally(t, t.seen, t.world, o.recorded[0] - 1) == mt::Verdict::KeptMismatch, "a stream that lost a kept draw is refused");
    CHECK(mt::CheckTally(t, t.seen, t.world, o.recorded[0] + 1) == mt::Verdict::KeptMismatch, "a stream with an extra draw is refused");
    mt::Tally none;
    CHECK(mt::CheckTally(none, 1, 1, 1) == mt::Verdict::Unknown, "no tally: unknown, not thin");
    World::Out ghost = o;
    ghost.frame = 999;
    CHECK(w.Check(ghost, 0) == mt::Verdict::Unknown, "a frame the hook did not note is unknown");
    mt::Tally full = t;
    full.dropped = 0;
    CHECK(mt::CheckTally(full, 1, 2, 3) == mt::Verdict::NotThin, "dropped 0: a full frame");
  }
  // 11. Stamp: a slot reused 64 frames later does not answer for the old frame.
  {
    mt::CountRing ring;
    if (Plant("stamp_ignored")) ring.honour_stamp = false;
    for (int i = 0; i < 10; ++i) ring.Note(100, mt::kStands, i != 0 && i % 3 != 0);
    ring.Note(100, mt::kFloor, true);
    uint32_t seen = 0, dropped = 0;
    uint64_t world = 0;
    CHECK(ring.Lookup(100, mt::kStands, seen, dropped, world) && seen == 10 && dropped == 6, "the stands tally counts every call and the dropped ones");
    CHECK(ring.Lookup(100, mt::kFloor, seen, dropped, world) && seen == 1 && dropped == 1, "the floor site has its own tally");
    CHECK(!ring.Lookup(101, mt::kStands, seen, dropped, world), "a frame the hook did not note is unknown");
    ring.Note(164, mt::kStands, false);
    CHECK(!ring.Lookup(100, mt::kStands, seen, dropped, world), "a slot reused 64 frames later no longer answers for the old frame");
  }
  // 12. Eligibility: a class without a palette (ours cannot draw it) is never dropped; a tiled frame is never thinned; an unknown class is never dropped.
  {
    World w;
    w.Warm(4);
    w.runs.store(1);
    std::vector<RunShape> s = World::Shape(2, 5, 0, 0);
    s[1].palette = false;
    const World::Out o = w.Run(mt::Mode::Skip, s);
    CHECK(o.dropped[0] == 4 && o.recorded[0] == 6, "a run whose class has no palette is recorded in full; the other is thinned");
    w.runs.store(1);
    s = World::Shape(2, 5, 0, 0);
    s[0].tiled = true;
    const World::Out t = w.Run(mt::Mode::Skip, s);
    CHECK(t.dropped[0] == 4 && t.recorded[0] == 6, "a tiled run is recorded in full");
    mt::Counters c;
    mt::Decider d;
    d.proven_run = true, d.stamp_run = true, d.proof_run = false, d.first[0] = false;
    mt::Query q;
    q.class_known = false;
    CHECK(!d.DropMemberDraw(mt::kStands, mt::Mode::Skip, w.policy, w.gate, c, w.now, q) && c.kept_why[0][mt::kUnknownClass].load() == 1, "a member whose class the observer could not decode is recorded");
  }
  // 13. Run boundaries: the first op 15 after each op 14 of the site is kept, per site, independent of the other site.
  {
    World w;
    w.Warm(4);
    w.runs.store(1);
    const World::Out o = w.Run(mt::Mode::Skip, {{mt::kStands, 3}, {mt::kFloor, 2}, {mt::kStands, 3}, {mt::kFloor, 1}});
    CHECK(o.recorded[0] == 2 && o.dropped[0] == 4, "two stands runs of 3: 2 kept, 4 dropped");
    CHECK(o.recorded[1] == 2 && o.dropped[1] == 1, "floor runs of 2 and 1: 2 kept, 1 dropped");
  }
  // 14. A gate that becomes proven mid-run does not change that run.
  {
    World w;
    w.runs.store(1);
    w.dec.BeginRun(w.policy, w.counters, w.runs, w.gate, w.now, true);   // cold gate
    uint32_t dropped = 0;
    for (int k = 0; k < 4; ++k) {
      w.dec.BeginSiteRun(mt::kStands, w.counters);
      for (int i = 0; i < 5; ++i) dropped += w.dec.DropMemberDraw(mt::kStands, mt::Mode::Skip, w.policy, w.gate, w.counters, w.now, mt::Query{});
      if (k == 1) for (int j = 0; j < 4; ++j) w.gate.Report(ct::Kind::FullOk, w.now);
    }
    CHECK(dropped == 0, "a gate that becomes proven mid-run does not thin the rest of that run");
  }
  // 15. SF1: a wrong stamp offset. Every thin frame is refused as a HOLE; they latch after 3.
  {
    World w;
    uint32_t thin_runs = 0;
    for (int i = 0; i < 80; ++i) {
      const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
      if (o.dropped[0] + o.dropped[1]) ++thin_runs;
      w.Bind(o, !(o.dropped[0] + o.dropped[1]));   // the binder cannot match a thin frame's tally: it is a hole
      w.now += 16;
    }
    CHECK(w.gate.latched.load(), "a wrong stamp offset latches the cut off after 3 holes");
    CHECK(thin_runs <= 3, "and no more than 3 thin runs ever happen");
  }

  // ---- Lane 295: the skip arm replayed. A recorded sequence is synthesised the way the census consumes it: per hook frame the tally the hook wrote, per stream frame (hook + 1) the record and the kept stream draws.
  // 16. A moving crowd, stamp at offset 1: nothing is dropped until the stamp is proven by full frames; then skip is TAKEN on thin frames, refused on proof frames (they are full), never a hole.
  {
    World w;
    w.use_stamp = true, w.moving = true;
    uint32_t thin = 0, full = 0, holes = 0, dropped_before_proof = 0, proof_full = 0;
    bool proven_seen = false;
    for (int i = 0; i < 200; ++i) {
      const bool was_proven = w.stamp.Proven(w.now) && w.gate.Proven(w.now);
      const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
      const World::Bound b = w.BindStream(o);
      if (!was_proven) dropped_before_proof += o.dropped[0] + o.dropped[1];
      proven_seen = proven_seen || was_proven;
      thin += b.j.take == mt::Take::Thin, full += b.j.take == mt::Take::Full, holes += b.j.take == mt::Take::Hole;
      if (o.proof) {
        proof_full += b.j.take == mt::Take::Full && b.j.ev == mt::StampEv::Only1;
        CHECK(o.dropped[0] + o.dropped[1] == 0, "replay: a proof run is recorded in full");
      }
      w.now += 16;
    }
    CHECK(proven_seen, "replay: the stamp and the gate get proven");
    CHECK(dropped_before_proof == 0, "replay: nothing is dropped before the offset-1 stamp is proven");
    CHECK(thin >= 150 && holes == 0, "replay: skip is taken on the offset-1 frames (>= 150 of 200), no hole");
    CHECK(proof_full >= 5, "replay: the proof frames are full frames judged offset 1 only");
    CHECK(w.stamp.only1.load() >= 3 && w.stamp.miss.load() == 0, "replay: the discriminating evidence is offset 1 only, no miss");
    CHECK(w.stamp.Proven(w.now), "replay: still proven at the end");
  }
  // 17. The stamp lands at offset 2 (a record two frames behind the hook) for a stretch: the Judge refuses every thin frame there (a hole, reason: offset 0 / 2 only), the stamp proof is voided at once,
  // and the hook records in full again (reason: stamp unproven) instead of dropping members ours cannot replace.
  {
    World w;
    w.use_stamp = true, w.moving = true;
    for (int i = 0; i < 60; ++i) {
      const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
      w.BindStream(o);
      w.now += 16;
    }
    CHECK(w.stamp.Proven(w.now) && w.gate.Proven(w.now), "elsewhere: proven before the shift");
    w.runs.store(1);   // not a proof run
    const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
    CHECK(o.dropped[0] + o.dropped[1] > 0, "elsewhere: the run before the shift is thin");
    const World::Bound b = w.BindStream(o, 2);   // the stream frame is hook + 2: offset 1 is a frame the hook has not recorded
    CHECK(b.j.take == mt::Take::Hole && b.j.why && std::string(b.j.why) == mt::kWhyElsewhere, "elsewhere: a thin frame whose stamp is at offset 2 only is refused (a hole, named)");
    CHECK(b.j.ev == mt::StampEv::Miss, "elsewhere: and it is a stamp miss");
    CHECK(!w.stamp.Proven(w.now), "elsewhere: the stamp proof is void after one miss");
    w.runs.store(1);
    w.now += 16;
    const World::Out o2 = w.Run(mt::Mode::Skip, World::Shape());
    CHECK(o2.dropped[0] + o2.dropped[1] == 0, "elsewhere: the hook records everything while the stamp is unproven");
    CHECK(w.counters.kept_why[0][mt::kStampUnproven].load() + w.counters.kept_why[0][mt::kNotProven].load() > 0 && (w.dec.last == mt::kStampUnproven || w.dec.last == mt::kNotProven), "elsewhere: and names a reason (the gate was reset by the hole, the stamp proof by the miss)");
    // offset 0 (the record of the SAME frame): the offset-1 tally is the previous, thin, frame: a world mismatch, refused too
    World w0;
    w0.use_stamp = true, w0.moving = true;
    for (int i = 0; i < 60; ++i) w0.BindStream(w0.Run(mt::Mode::Skip, World::Shape())), w0.now += 16;
    w0.runs.store(1);
    const World::Out p = w0.Run(mt::Mode::Skip, World::Shape());
    const World::Bound b0 = w0.BindStream(p, 0);
    CHECK(b0.j.take == mt::Take::Hole && b0.j.ev == mt::StampEv::Miss, "elsewhere: a stamp at offset 0 only is refused too (the neighbour's tally does not match the record)");
  }
  // 18. Unproven: a proven gate with no stamp evidence drops nothing; a stamp proof goes stale; a STATIC crowd (every offset matches: ambiguous) never proves offset 1.
  {
    World w;
    w.use_stamp = true, w.moving = true;
    for (int i = 0; i < 6; ++i) w.gate.Report(ct::Kind::FullOk, w.now);
    w.runs.store(1);
    const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
    CHECK(o.dropped[0] + o.dropped[1] == 0 && w.counters.kept_why[0][mt::kStampUnproven].load() > 0, "unproven: a proven gate without stamp evidence drops nothing");
    for (int i = 0; i < 20; ++i) w.BindStream(w.Run(mt::Mode::Skip, World::Shape())), w.now += 16;
    CHECK(w.stamp.Proven(w.now), "unproven: evidence proves it");
    w.now += 1000;
    CHECK(!w.stamp.Proven(w.now), "unproven: a silent second voids it");
    World s;
    s.use_stamp = true, s.moving = false;
    for (int i = 0; i < 60; ++i) s.BindStream(s.Run(mt::Mode::Skip, World::Shape())), s.now += 16;
    CHECK(s.stamp.ambiguous.load() > 0 && s.stamp.only1.load() <= 1, "ambiguous: a static crowd matches offsets 0, 1 and 2 alike");
    CHECK(!s.stamp.Proven(s.now), "ambiguous: it never proves offset 1");
    s.runs.store(1);
    const World::Out so = s.Run(mt::Mode::Skip, World::Shape());
    CHECK(so.dropped[0] + so.dropped[1] == 0, "ambiguous: nothing is dropped on it");
  }
  // 19. The verify arm: full frames of an honest sequence check clean; one planted bad record (a world sum off by one) is counted as exactly one differing calls check; a uniformity compare counts material / bank.
  {
    World w;
    w.use_stamp = false, w.moving = true;
    mt::VerifyArm total;
    for (int i = 0; i < 40; ++i) {
      w.runs.store(1);
      const World::Out o = w.Run(mt::Mode::Census, World::Shape());   // census: everything recorded, a full frame
      const World::Bound b = w.BindStream(o, 1, i == 20 ? 1 : 0);   // frame 20: a record whose members' world sum is off by one
      total.calls_checked += b.va.calls_checked, total.calls_differ += b.va.calls_differ;
      w.now += 16;
    }
    CHECK(total.calls_checked == 80, "verify: 2 sites x 40 full frames checked");
    CHECK(total.calls_differ == 2, "verify: the planted bad record is counted (2 sites)");
    mt::VerifyArm va;
    va.Member(true, true), va.Member(true, true), va.Member(false, true), va.Member(true, false);
    va.Unsourced(3);
    CHECK(va.members == 4 && va.mismatching() == 2 && va.material_differs == 1 && va.bank_differs == 1 && va.unsourced == 3, "verify: members, material / bank differs and unsourced are counted");
  }

  // ---- Lane 295 review round: a LAGGED replay (the binder judges the hook's frame D = 2 frames after the hook recorded it, so the hook is ahead and the offset-0 slot exists).
  struct Lagged {
    World& w;
    std::deque<World::Out> q;
    size_t lag;
    uint64_t skew = 1;
    uint32_t hook_thin_runs = 0;
    bool judged = false;
    World::Bound Step() {
      const World::Out o = w.Run(mt::Mode::Skip, World::Shape());
      hook_thin_runs += (o.dropped[0] + o.dropped[1]) != 0;
      q.push_back(o);
      World::Bound b;
      judged = false;
      if (q.size() > lag) {
        const World::Out j = q.front();
        q.pop_front();
        b = w.BindStream(j, skew);
        judged = true;
      }
      w.now += 16;
      return b;
    }
  };
  // 20a. The stamp shifts to offset 2 for 20 frames (the binder lags the hook by D = 2): no thin frame is taken while shifted, at most D + 1 holes (and thin hook runs) before the first unproven run, every full frame
  // judged during the shift is a Miss, and the stamp is proven again only after 3 offset-1-only frames. Plant judge_count_only (the world sum ignored) takes shifted thin frames: it must fail this group.
  {
    World w;
    w.use_stamp = true, w.moving = true;
    Lagged L{w, {}, 2};
    for (int i = 0; i < 60; ++i) L.Step();
    CHECK(w.stamp.Proven(w.now), "lagged: proven before the shift");
    L.skew = 2, L.hook_thin_runs = 0;
    uint32_t holes = 0, thin = 0, full_not_miss = 0, full_miss = 0;
    for (int i = 0; i < 20; ++i) {
      const World::Bound b = L.Step();
      if (!L.judged) continue;
      thin += b.j.take == mt::Take::Thin, holes += b.j.take == mt::Take::Hole;
      if (b.j.take == mt::Take::Full) (b.j.ev == mt::StampEv::Miss ? full_miss : full_not_miss)++;
    }
    CHECK(thin == 0, "lagged: no thin frame is taken while the stamp is shifted");
    CHECK(holes <= 3 && L.hook_thin_runs <= 3, "lagged: at most D + 1 holes (and thin hook runs) before the first unproven run");
    CHECK(full_miss > 0 && full_not_miss == 0, "lagged: every full frame judged while shifted is a Miss");
    CHECK(!w.stamp.Proven(w.now), "lagged: the stamp is unproven after the shift");
    L.skew = 1;
    uint32_t only1 = 0;
    bool order_ok = true;
    for (int i = 0; i < 8; ++i) {
      const World::Bound b = L.Step();
      if (!L.judged) continue;
      only1 += b.j.ev == mt::StampEv::Only1;
      if (w.stamp.Proven(w.now) != (only1 >= 3)) order_ok = false;
    }
    CHECK(order_ok && only1 >= 3, "lagged: proven again only after 3 offset-1-only frames");
  }
  // 20b. Silence of 300 ms, then a STATIC crowd with the stamp shifted (every offset matches: no offset-1-only evidence): nothing is dropped before 3 NEW offset-1-only frames. The old Report kept the previous
  // scene's streak across the silence and re-proved on the first report (SF1: plant stale_streak_kept).
  {
    World w;
    w.use_stamp = true, w.moving = true;
    Lagged L{w, {}, 2};
    for (int i = 0; i < 60; ++i) L.Step();
    CHECK(w.stamp.Proven(w.now), "silence: proven before the pause");
    w.now += 300;
    L.q.clear();   // (the frames in flight were from the previous scene)
    w.moving = false;
    L.skew = 2, L.hook_thin_runs = 0;
    for (int i = 0; i < 12; ++i) L.Step();
    CHECK(L.hook_thin_runs == 0, "silence: a 300 ms pause voids the proof: nothing is dropped on the static, shifted crowd that follows");
    mt::Gate stale;   // the same class on the gate (card_thin Gate, used by the members): the first report after silence does not re-prove
    for (int i = 0; i < 4; ++i) stale.Report(ct::Kind::FullOk, 1000 + i);
    CHECK(stale.Proven(1004), "silence: gate proven");
    stale.Report(ct::Kind::FullOk, 1004 + 400);
    CHECK(!stale.Proven(1404), "silence: the first report after 400 ms does not re-prove the gate");
  }

  std::printf(g_fail ? "member_thin_test: %d FAIL\n" : "member_thin_test: all pass\n", g_fail);
  return g_fail ? 1 : 0;
}
