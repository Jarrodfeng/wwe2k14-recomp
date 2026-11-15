// S102 lane R24-E: unit test of rdr::GpuSmallDecide (src/renderer/gpu_small.h). Built and run by tools\r24e\gpu_small_check.py. Exit 1 on any wrong case.
// Each case: (mask, DOFParam2.x, far blur separate, bloom / glare reads the chain, viewer depth, has dof chain) -> the answer. Then the planted faults: each must change at least one answer.
#include <cmath>
#include <cstdio>

#include "../../src/renderer/gpu_small.h"

namespace {
struct Case { int mask; float x; bool far_separate, reads, viewer, has; rdr::GpuSmallWhy want; const char* name; bool clamped = true; };
const Case kCases[] = {
    {1, 0.f, true, false, false, true, rdr::kGsTaken, "Bryan's entrance: x 0, far blur an earlier layer, no bloom: the chain is dead"},
    {0, 0.f, true, false, false, true, rdr::kGsOff, "switch off"},
    {2, 0.f, true, false, false, true, rdr::kGsOff, "bit 2 alone is reserved and does nothing"},
    {4, 0.f, true, false, false, true, rdr::kGsOff, "bit 4 alone is reserved and does nothing"},
    {1, 0.99f, true, false, false, true, rdr::kGsNearLive, "a match: the near blur is live (x 0.99)"},
    {1, 1e-6f, true, false, false, true, rdr::kGsNearLive, "x a hair above 0 is live (the near branch runs for a pixel with depth 1)"},
    {1, -0.5f, true, false, false, true, rdr::kGsTaken, "a negative x never runs the near branch"},
    {1, 0.f, false, false, false, true, rdr::kGsFarShared, "x 0 but slot 6 reads the same blur: kept"},
    {1, 0.f, true, true, false, true, rdr::kGsBloomLive, "bloom / glare reads the chain: kept"},
    {1, 0.f, true, false, true, true, rdr::kGsViewerDepth, "the viewer's converted depth can exceed 1: kept"},
    {1, 0.f, true, false, false, false, rdr::kGsNoDofChain, "no dof chain in this record"},
    {1, 0.f, true, false, false, true, rdr::kGsDepthFloat, "float colour depth (R32F / RGBA16F: values may exceed 1): kept", false},
    {1, std::nanf(""), true, false, false, true, rdr::kGsTaken, "x NaN: `NaN > behind` is false in the shader, the near branch never runs"},
    {1, -0.f, true, false, false, true, rdr::kGsTaken, "x -0.0: not greater than any behind >= 0"},
};
}  // namespace

int main() {
  int fails = 0;
  for (int plant = 0; plant <= 5; ++plant) {
    rdr::g_gpu_small_plant.store(plant);
    int changed = 0;
    for (const Case& c : kCases) {
      const rdr::GpuSmallWhy got = rdr::GpuSmallDecide(c.mask, c.x, c.far_separate, c.reads, c.viewer, c.has, c.clamped);
      if (plant == 0) {
        const bool ok = got == c.want;
        std::printf("%s plant 0: %s (%d, want %d)\n", ok ? "ok  " : "FAIL", c.name, int(got), int(c.want));
        fails += ok ? 0 : 1;
      } else if (got != c.want) {
        ++changed;
      }
    }
    if (plant > 0) {
      const bool ok = changed > 0;   // a planted fault that changes no answer is a twin that cannot fail: the test must see it
      std::printf("%s plant %d changes %d answers (must be > 0)\n", ok ? "ok  " : "FAIL", plant, changed);
      fails += ok ? 0 : 1;
    }
  }
  rdr::g_gpu_small_plant.store(0);
  std::printf(fails ? "FAILED %d\n" : "all passed\n", fails);
  return fails ? 1 : 0;
}
