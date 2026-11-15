// S102 lane R24-F: offline unit test of src/renderer/group_stamps.h (the REAL header, no Vulkan, no game): the segment planner the recorder runs per draw, the chain check and the
// per-frame aggregation the collector runs. Every planted fault must change an answer. Exit 1 on any wrong case.
//   clang++ -std=c++20 -Wall -Wextra -Werror tools/r24f/group_stamps_test.cpp -o group_stamps_test.exe
#include <cstdio>
#include <vector>

#include "../../src/renderer/group_stamps.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace rdr;

// Runs the recorder's loop over a draw list: returns the segments (the last one closed like RecordPass does) and how many stamps were written.
struct Run {
  std::vector<GroupSeg> segs;
  uint32_t stamps = 0;
  uint32_t draws = 0;
};
static Run Plan(const std::vector<uint8_t>& groups, uint32_t min_run, uint32_t budget, int plant = 0, uint32_t first_begin = 100, uint32_t end = 105) {
  Run r;
  SegmentPlanner p;
  p.min_run = min_run, p.budget = budget, p.last = kGrpIdle;
  uint32_t prev = first_begin;
  uint32_t ordinal = 0;
  for (uint8_t g : groups) {
    if (p.NeedStamp(g)) {
      GroupSeg s;
      const uint32_t enc = kPoolBit | r.stamps;
      s.begin = prev, s.end = enc;
      p.Close(s);
      if (!(plant == 1 && r.stamps == 2)) r.segs.push_back(s);
      prev = enc;
      ++r.stamps, --p.budget;
    }
    if (!(plant == 2 && ordinal == 4)) p.Add(g);
    else p.last = g;
    ++ordinal;
    ++r.draws;
  }
  GroupSeg last;
  last.begin = prev, last.end = end;
  p.Close(last);
  r.segs.push_back(last);
  return r;
}

static uint32_t Total(const Run& r) {
  uint32_t n = 0;
  for (const GroupSeg& s : r.segs)
    for (uint32_t c : s.counts) n += c;
  return n;
}

static double Span(uint32_t a, uint32_t b) {   // a fake clock: main stamp i is at 10 ns x i (the pass is 100..105 = 1000..1050 ns); group stamp q sits at 1000 + 5 (q + 1)
  const auto t = [](uint32_t id) { return (id & kPoolBit) ? 1000.0 + 5.0 * double((id & ~kPoolBit) + 1) : 10.0 * id; };
  return t(b) - t(a);
}

int main() {
  const std::vector<uint8_t> w = {kGrpArena, kGrpArena, kGrpWrestler, kGrpArena, kGrpArena, kGrpArena, kGrpCrowd, kGrpCrowd, kGrpCrowd, kGrpCrowd, kGrpRing, kGrpParticle, kGrpParticle};  // 13 draws
  // 1. min run 1: a stamp at every change (6 changes: A A | W | A A A | C C C C | R | P P)
  Run r1 = Plan(w, 1, 100);
  CHECK(r1.stamps == 5, "min run 1: %u stamps, want 5 (changes: W, A, C, R, P)", r1.stamps);
  CHECK(r1.segs.size() == 6, "segments %zu, want stamps + 1", r1.segs.size());
  CHECK(Total(r1) == r1.draws, "every draw is in exactly one segment (%u of %u)", Total(r1), r1.draws);
  CHECK(r1.segs[0].dominant == kGrpArena && r1.segs[1].dominant == kGrpWrestler && r1.segs[3].dominant == kGrpCrowd, "dominant groups of the first segments");
  // 2. min run 3: short segments are merged into the next ones (A A W A A A | C C C C | R P P): only boundaries after >= 3 draws stamp
  Run r3 = Plan(w, 3, 100);
  CHECK(r3.stamps < r1.stamps, "min run 3 writes fewer stamps (%u) than min run 1 (%u)", r3.stamps, r1.stamps);
  CHECK(Total(r3) == r3.draws, "min run 3: every draw is in exactly one segment (%u of %u)", Total(r3), r3.draws);
  // 3. budget: a pool with room for 2 stamps cuts twice; the rest of the pass is one mixed segment, never an overflow, and still holds every draw
  Run rb = Plan(w, 1, 2);
  CHECK(rb.stamps == 2 && Total(rb) == rb.draws, "budget 2: %u stamps, %u of %u draws held", rb.stamps, Total(rb), rb.draws);
  // 4. a pass with no draws is one segment labelled "no_draws"
  Run re = Plan({}, 1, 10);
  CHECK(re.segs.size() == 1 && re.segs[0].dominant == kGrpIdle && Total(re) == 0, "an empty pass: one idle segment");
  // 5. the chain is continuous and its spans add up to the pass's span (end - begin)
  const auto chain = [&](const Run& r) { return CheckChain(r.segs.data(), uint32_t(r.segs.size()), 100, 105, 1e9, Span); };
  ChainResult c1 = chain(r1);
  CHECK(c1.broken == 0, "a clean chain is not broken (%u)", c1.broken);
  CHECK(c1.sum_ns == Span(100, 105), "the spans add up to the pass's own span (%.1f vs %.1f)", c1.sum_ns, Span(100, 105));
  // 6. PLANT 1: a stamp whose segment was not recorded: the chain check must see it
  Run p1 = Plan(w, 1, 100, 1);
  CHECK(chain(p1).broken > 0, "plant 1 (a stamp written, its segment missing): the chain check must report it");
  // 7. PLANT 2: a draw left out of the counts: the coverage (segments vs the draws issued) must differ
  Run p2 = Plan(w, 1, 100, 2);
  CHECK(Total(p2) + 1 == p2.draws, "plant 2 (a draw left out of the counts): held %u of %u, must be one short", Total(p2), p2.draws);
  // 8. a wrapped / absurd span is flagged by the limit
  Run rw = Plan(w, 1, 100);
  CHECK(CheckChain(rw.segs.data(), uint32_t(rw.segs.size()), 100, 105, 5.0, Span).broken > 0, "a limit below the spans flags them (wrapped ticks)");
  // 9. a chain that ends elsewhere than the pass's closing stamp is broken
  CHECK(CheckChain(rw.segs.data(), uint32_t(rw.segs.size()), 100, 106, 1e9, Span).broken > 0, "a chain that does not end at the closing stamp is broken");
  // 10. aggregation: ns go to the dominant group, draws to their own groups, and the mixed draws are the ones in a segment labelled with another group
  PassGroups pg;
  for (const GroupSeg& s : r3.segs) Aggregate(s, 10.0, pg);
  uint32_t dsum = 0;
  for (uint32_t d : pg.draws) dsum += d;
  CHECK(dsum == r3.draws && pg.total_draws == r3.draws, "aggregate: the per-group draws add up to the pass's draws (%u of %u)", dsum, r3.draws);
  CHECK(pg.mixed_draws > 0, "min run 3 merges short runs: some draws sit in a segment labelled with another group");
  PassGroups pg1;
  for (const GroupSeg& s : r1.segs) Aggregate(s, 10.0, pg1);
  CHECK(pg1.mixed_draws == 0, "min run 1 cuts at every change: no draw sits in a segment of another group (%u)", pg1.mixed_draws);
  double ns = 0;
  for (double x : pg1.ns) ns += x;
  CHECK(ns == 10.0 * double(r1.segs.size()), "the per-group ns add up to the segments' ns");
  // 11. group names: one per id
  for (uint32_t g = 0; g < kGrpCount; ++g) CHECK(GroupName(g)[0] != '?', "group %u has a name", g);
  CHECK(GroupName(kGrpCount)[0] == '?', "an id past the end is '?'");
  std::printf(g_fail ? "FAILED %d\n" : "all passed (group_stamps.h: planner, budget, chain, coverage, aggregation, both planted faults seen)\n", g_fail);
  return g_fail ? 1 : 0;
}
