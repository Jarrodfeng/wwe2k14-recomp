// S110 lane DETLOADS: the shipped det rules for the load speed-ups (src/det_loads.h, the pure part: no SDK, no game).
// Truth table of the three predicates over det on/off x replay loads fast/slow, GuestFrameCap, and a planted twin.
//   clang++ -std=c++17 -DDET_LOADS_PURE -I src tools/report/det_loads_unit.cpp -o det_loads_unit.exe
//   det_loads_unit.exe          -> exit 0 when every case holds
//   det_loads_unit.exe plant    -> the planted twin (the wall cap still applied under det + fast): must exit 1 (the table catches it)
#include <cstdio>
#include <cstring>

#include "det_loads.h"

using namespace wwe2k14::det_loads;

static int g_fails = 0;
static void Expect(bool ok, const char* what, bool det, bool fast) {
  if (!ok) {
    ++g_fails;
    std::printf("FAIL %s (det %d, fast %d)\n", what, det, fast);
  }
}

// The planted twin: the wall-clock rule kept under det + fast (what the code did before the build).
static bool PlantedWallCap(bool, bool) { return true; }

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "plant") == 0;
  for (int d = 0; d < 2; ++d) {
    for (int f = 0; f < 2; ++f) {
      const bool det = d, fast = f;
      const bool want = !(det && fast);  // false only when det is on AND fast replay loads are chosen
      Expect((plant ? PlantedWallCap(det, fast) : WallCapApplies(det, fast)) == want, "WallCapApplies", det, fast);
      Expect(UiThrottleApplies(det, fast) == want, "UiThrottleApplies", det, fast);
      Expect(FastLoaderBudgetApplies(det, fast) == want, "FastLoaderBudgetApplies", det, fast);
    }
  }
  // The cap in guest frames: the 8 s default at 60 Hz = 480; other values round up; no cap = 0.
  Expect(GuestFrameCap(8000) == 480, "GuestFrameCap(8000) == 480", true, true);
  Expect(GuestFrameCap(8000, 30) == 240, "GuestFrameCap(8000, 30) == 240", true, true);
  Expect(GuestFrameCap(1) == 1, "GuestFrameCap(1) rounds up to 1", true, true);
  Expect(GuestFrameCap(0) == 0 && GuestFrameCap(-5) == 0, "GuestFrameCap(<=0) == 0", true, true);
  std::printf(g_fails ? "det_loads_unit: %d FAILED\n" : "det_loads_unit: all passed\n", g_fails);
  return g_fails ? 1 : 0;
}
