// S103 SPBF review M1: the unit test of src\renderer_scene\crowd_density.h (the stream-free crowd's "is this figure drawn" decision). No game, no SDK.
//   crowd_density_test.exe [--plant old_early_return]      exit 0 = all pass, 1 = a check failed (the plant must exit 1)
// What it proves, over 2,000 static figures ranked by the real tracker (crowd_rank.h, the same one live_crowd.cpp and the stream binder use):
//   1. density 100: every card and every member is drawn (no rank is asked: the default costs nothing).
//   2. density 50: the cards are culled the way the members are: the same figures (equal rank) are kept / left out, and about half of each are drawn (the review's finding: cards ignored the density).
//   3. density 0: nothing is drawn. no_cards / no_members: Skipped (a skip, not a cull) at any density, before any rank is asked.
//   4. the stream binder's rule is the same: kept = rank < density.
// Plant old_early_return: the first stream-free version (cards return before the density test): check 2 must fail.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/crowd_rank.h"
#include "../../src/renderer_scene/crowd_density.h"

using namespace rdr_scene::crowd_density;

static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
static uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

// The old rule, for the plant: the card branch returned before the density test.
template <class RankFn>
static Figure DecideOld(bool card, bool no_cards, bool no_members, int density, RankFn rank) {
  if (card) return no_cards ? Figure::Skipped : Figure::Drawn;
  return Decide(card, no_cards, no_members, density, rank);
}

int main(int argc, char** argv) {
  const bool plant = argc > 2 && std::string(argv[1]) == "--plant" && std::string(argv[2]) == "old_early_return";
  const auto decide = [&](bool card, bool nc, bool nm, int d, auto rank) { return plant ? DecideOld(card, nc, nm, d, rank) : Decide(card, nc, nm, d, rank); };
  rdr_adapter::CrowdRankTracker tracker;
  tracker.NextFrame(1);
  const int kN = 2000;
  std::vector<int> ranks;
  for (int i = 0; i < kN; ++i) ranks.push_back(int(tracker.Rank(Bits(float(i) * 1.7f), Bits(float(i % 37) * 0.9f), Bits(float(i / 37) * 2.3f))));

  int asked = 0;
  int cards100 = 0, members100 = 0;
  for (int i = 0; i < kN; ++i) {
    const auto rk = [&] { ++asked; return ranks[size_t(i)]; };
    cards100 += decide(true, false, false, 100, rk) == Figure::Drawn;
    members100 += decide(false, false, false, 100, rk) == Figure::Drawn;
  }
  Check(cards100 == kN && members100 == kN && asked == 0, "density 100: all " + std::to_string(kN) + " cards and members drawn, no rank asked (asked " + std::to_string(asked) + ")");

  int cards50 = 0, members50 = 0, mismatch = 0, binder_mismatch = 0;
  for (int i = 0; i < kN; ++i) {
    const auto rk = [&] { return ranks[size_t(i)]; };
    const Figure c = decide(true, false, false, 50, rk), m = decide(false, false, false, 50, rk);
    cards50 += c == Figure::Drawn, members50 += m == Figure::Drawn;
    mismatch += c != m;
    binder_mismatch += (m == Figure::Drawn) != (ranks[size_t(i)] < 50);
  }
  std::printf("     density 50: cards drawn %d / %d, members drawn %d / %d, card-vs-member differences %d\n", cards50, kN, members50, kN, mismatch);
  Check(mismatch == 0 && cards50 > kN * 40 / 100 && cards50 < kN * 60 / 100, "density 50 culls the cards like the members: same figures, about half drawn");
  Check(binder_mismatch == 0, "the rule is the binder's: drawn exactly when rank < density");

  int any0 = 0, skipped_nc = 0, skipped_nm = 0, asked_skip = 0;
  for (int i = 0; i < kN; ++i) {
    const auto rk = [&] { return ranks[size_t(i)]; };
    const auto rk2 = [&] { ++asked_skip; return ranks[size_t(i)]; };
    any0 += decide(true, false, false, 0, rk) == Figure::Drawn || decide(false, false, false, 0, rk) == Figure::Drawn;
    skipped_nc += decide(true, true, false, 50, rk2) == Figure::Skipped;
    skipped_nm += decide(false, false, true, 50, rk2) == Figure::Skipped;
  }
  Check(any0 == 0, "density 0: nothing drawn");
  Check(skipped_nc == kN && skipped_nm == kN && asked_skip == 0, "no_cards / no_members: Skipped (a skip, not a cull), no rank asked");
  std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
