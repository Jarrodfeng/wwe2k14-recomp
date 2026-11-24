// Unit for src\owner_timing_det.h (S110 DETBOOT): the truth table of owner_det::Ready, plus a planted twin that ignores the gate.
// Build: clang++ -std=c++20 -DOWNER_DET_PURE -I src tools/owner/owner_det_unit.cpp -o out/tools/owner_det_unit.exe
// Run:   out\tools\owner_det_unit.exe        (exit 0)      out\tools\owner_det_unit.exe plant   (exit 1: the planted twin fails)
#include <cstdio>
#include <cstring>

#include "owner_timing_det.h"

static int g_fails = 0;
static void Expect(bool ok, const char* what, bool items, bool stop, bool gate, unsigned word) {
  if (!ok) {
    ++g_fails;
    std::printf("FAIL %s items=%d stop=%d gate=%d word=%u\n", what, items, stop, gate, word);
  }
}

// the planted twin: ignores the gate
static constexpr bool ReadyPlanted(bool has_items, bool stop, bool, unsigned) { return has_items || stop; }

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "plant") == 0;
  for (int m = 0; m < 16; ++m) {
    const bool items = m & 1, stop = m & 2, gate = m & 4;
    const unsigned word = (m & 8) ? 5u : 0u;
    const bool want = items || stop || (gate && word == 0);
    const bool got = plant ? ReadyPlanted(items, stop, gate, word)
                           : wwe2k14::owner_det::Ready(items, stop, gate, word);
    Expect(got == want, "Ready", items, stop, gate, word);
  }
  std::printf("owner_det_unit: %s (%d fails)\n", g_fails ? "FAIL" : "ok", g_fails);
  return g_fails ? 1 : 0;
}
