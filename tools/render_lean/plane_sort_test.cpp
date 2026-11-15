// S102 RT1 (Q9): plane::Board::Add with the sort only where needed (render_lean bit 4) against the shipped Add (every kept frame's list sorted every call): after every Add of a long random sequence
// (batches for the same stamp arriving out of ordinal order, repeated stamps, many stamps) both boards must answer Find() identically for every (stamp, kind, nth). `plant4` = a touched list is not sorted.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../../src/renderer_scene/plane_shadow_core.h"

using namespace rdr_scene;

static bool Same(const plane::Board& a, const plane::Board& b, uint32_t lo, uint32_t hi) {
  for (uint32_t stamp = lo; stamp <= hi; ++stamp)
    for (uint32_t kind = 0; kind < 2; ++kind)
      for (uint32_t nth = 0; nth < 12; ++nth) {
        const PlaneDraw* x = a.Find(stamp, kind, 3, 1, nth);
        const PlaneDraw* y = b.Find(stamp, kind, 3, 1, nth);
        if ((x == nullptr) != (y == nullptr)) return false;
        if (x && x->ordinal != y->ordinal) return false;
      }
  return a.draw_count() == b.draw_count();
}

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::string(argv[1]) == "plant4";
  std::mt19937 rng(777);
  plane::Board off, on;
  int bad = 0;
  uint64_t before_skip = 0, before_run = 0;
  for (int call = 0; call < 300; ++call) {
    std::vector<PlaneFrame> frames;
    std::vector<PlaneDraw> draws;
    const uint32_t stamp = 100 + uint32_t(call / 3) + uint32_t(rng() % 2);
    const int n = 1 + int(rng() % 8);
    for (int i = 0; i < n; ++i) {
      PlaneDraw d;
      d.frame = stamp - (rng() % 5 == 0 ? 1 : 0);
      d.kind = kPlaneMask;
      d.object = 3, d.part = 1;
      d.ordinal = uint32_t(rng() % 10);
      draws.push_back(d);
    }
    off.Add(frames, draws);
    rdr::g_lean_hooks.store(rdr::kLeanHandoff);
    rdr::g_lean_plant.store(plant ? 4 : 0);
    on.Add(frames, draws);
    rdr::g_lean_hooks.store(0);
    if (!Same(off, on, 90, 220) && ++bad == 1) std::printf("DIFF at call %d\n", call);
  }
  before_skip = rdr::LeanGet(rdr::kLcSortSkip), before_run = rdr::LeanGet(rdr::kLcSortRun);
  std::printf("%s: 300 Add calls, %d differing, sorts skipped %llu run %llu\n", plant ? "plant4" : "real", bad, static_cast<unsigned long long>(before_skip), static_cast<unsigned long long>(before_run));
  if (!plant && before_skip + before_run == 0) { std::printf("FAIL the lean path never ran\n"); return 1; }
  return bad ? 1 : 0;
}
