// lane 168: the rate decision on planted readings. clang++ -std=c++17 -DWWE2K14_RATE_PURE -I src/renderer_adapter src/renderer_adapter/display_rate_test.cpp
#include <cstdio>
#include <limits>
#include "display_rate.h"

static int fails = 0;
static void Case(const char* name, double panel, int requested, uint32_t want_hz, bool want_tween, bool want_known) {
  const auto d = rdr_adapter::rate::Decide(panel, requested);
  const bool ok = d.pump_hz == want_hz && d.tween == want_tween && d.panel_known == want_known;
  std::printf("%s %-36s panel %8.2f requested %3d -> %u Hz tween %d known %d (%s)\n", ok ? "PASS" : "FAIL", name, panel, requested, d.pump_hz,
              d.tween, d.panel_known, d.why);
  if (!ok) ++fails;
}

int main() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  Case("60 Hz panel, 120 asked", 60.0, 120, 60, false, true);
  Case("59.94 Hz panel, 120 asked", 59.94, 120, 60, false, true);
  Case("120 Hz panel, 120 asked", 120.0, 120, 120, true, true);
  Case("119.88 Hz panel, 120 asked", 119.88, 120, 120, true, true);
  Case("144 Hz panel, 120 asked", 144.0, 120, 120, true, true);
  Case("144 Hz panel, UNCAPPED (144 asked)", 144.0, 144, 120, true, true);
  Case("90 Hz panel, 120 asked", 90.0, 120, 60, false, true);
  Case("60 setting on 120 Hz panel", 120.0, 60, 60, false, false);
  Case("unknown refresh (0): request kept", 0.0, 120, 120, true, false);
  Case("wrong read 5 Hz: unknown", 5.0, 120, 120, true, false);
  Case("wrong read 9999 Hz: unknown", 9999.0, 120, 120, true, false);
  Case("wrong read NaN: unknown", nan, 120, 120, true, false);
  Case("wrong read negative: unknown", -60.0, 120, 120, true, false);
  std::printf("%s\n", fails ? "FAILED" : "all passed");
  return fails;
}
