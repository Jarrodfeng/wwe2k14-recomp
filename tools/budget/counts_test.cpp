// S102 lane M1: unit test of src/renderer/counts.h (no Vulkan). Built and run by tools\budget\counts_unit.py.
#include <cstdio>
#include "../../src/renderer/counts.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s (line %d)\n", #c, __LINE__); ++fails; } } while (0)

int main() {
  using namespace rdr;
  // review C: one per attachment whose load op is CLEAR
  CHECK(LoadClearCount(0, true) == 1);          // LOAD colour + CLEAR depth -> 1 (was 2: the old code counted every attachment of a pass that cleared any)
  CHECK(LoadClearCount(1, false) == 1);         // CLEAR colour 0, no depth
  CHECK(LoadClearCount(0b101, true) == 3);
  CHECK(LoadClearCount(0b1111, false) == 4);
  CHECK(LoadClearCount(0, false) == 0);         // nothing cleared
  // Reset leaves nothing from the frame before (review F: a dropped frame must not read as the one before it)
  FrameCounts f;
  f.passes = 7, f.dropped = 1, f.total.draws = 9, f.upload_bytes = 5;
  f.pass.push_back(PassCounts{});
  f.Reset(12);
  CHECK(f.frame == 12 && f.passes == 0 && f.dropped == 0 && f.total.draws == 0 && f.upload_bytes == 0 && f.pass.empty());
  // CallCounts::Add sums every field
  CallCounts a, b;
  a.draws = 1, a.area_px = 10, a.barrier_images = 2;
  b.draws = 2, b.area_px = 5, b.barrier_images = 3, b.indices = 7;
  a.Add(b);
  CHECK(a.draws == 3 && a.area_px == 15 && a.barrier_images == 5 && a.indices == 7);
  std::printf(fails ? "counts_test: FAIL\n" : "counts_test: PASS\n");
  return fails ? 1 : 0;
}
