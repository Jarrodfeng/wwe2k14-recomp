// Offline proof of the missing-bits histogram (S103 X1, src\renderer_game\scene_frame.h missing_hist): synthetic frames through the REAL DecideNow, no game, no run.
// A frame missing bits {3,4} is counted under {3,4} (never under bit 3 or bit 4 alone), windows drain, the sum check holds, and the planted twin (a set counted under the
// wrong key: its lowest bit) is caught.
//   pwsh -NoProfile -File tools\scene\build_x1_hist_unit.ps1  ->  out\tools\x1_hist_unit.exe
#define RDR_X1_PLANTS 1
#include <cstdio>
#include <string>

#include "../../src/renderer_game/scene_frame.h"

namespace sf = scene_frame;
namespace mh = scene_frame::missing_hist;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool Has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

// Frames [from, to): the inputs in `stamped` carry a stamp for the frame, the rest do not (required = bits 3, 4, 10).
static void Frames(uint64_t from, uint64_t to, uint32_t stamped) {
  for (uint64_t f = from; f < to; ++f) {
    for (int b = 0; b < 32; ++b)
      if ((stamped >> b) & 1u) rdr_own::InputCaptured(rdr_own::Pass::Scene3D, b, f);
    (void)sf::DecideNow(f);
  }
}

int main() {
  static const int mode2 = 2;
  sf::g_mode_cvar.store(&mode2);
  sf::SnapshotMode();
  for (int b : {3, 4, 10}) sf::RequireInputLag(b, 0);
  sf::SetReady(true);

  // 1. 900 frames missing {3,4} (only 10 stamped), 300 missing {10} (3, 4 stamped), 50 missing {3,4,10}, 40 complete (ours).
  Frames(0, 900, 1u << 10);
  Frames(900, 1200, (1u << 3) | (1u << 4));
  Frames(1200, 1250, 0);
  Frames(1250, 1290, (1u << 3) | (1u << 4) | (1u << 10));
  std::string l = mh::MissingLine();
  std::printf("%s\n", l.c_str());
  CHECK(Has(l, "hook frames 0-1289"));
  CHECK(Has(l, "decided 1290, ours 40, inputs_missing 1250, other 0"));
  CHECK(Has(l, "sets {3,4}:900 {10}:300 {3,4,10}:50 (sum ok)"));
  CHECK(!Has(l, "SUM MISMATCH"));
  CHECK(!Has(l, "{3}:") && !Has(l, "{4}:"));

  // 2. the window drained: the next line starts at zero
  l = mh::MissingLine();
  CHECK(Has(l, "decided 0, ours 0, inputs_missing 0, other 0; sets - (sum ok)"));
  CHECK(Has(l, "window 2"));

  // 3. a held frame is "other", not a set; a window of one set
  Frames(2000, 2010, (1u << 3) | (1u << 10));   // missing {4}
  rdr_own::S(rdr_own::Pass::Scene3D).hold_until.store(2012);
  Frames(2010, 2012, 0);                        // hold: the inputs are missing too, but the first reason is the hold
  l = mh::MissingLine();
  CHECK(Has(l, "decided 12, ours 0, inputs_missing 10, other 2; sets {4}:10 (sum ok)"));

  // 4. the planted twin: a set counted under the wrong key (its lowest bit) is caught: {3,4} merges into {3}
  mh::g_plant = 1;
  Frames(3000, 3100, 1u << 10);                 // missing {3,4}
  Frames(3100, 3150, (1u << 4) | (1u << 10));   // missing {3}
  l = mh::MissingLine();
  std::printf("planted: %s\n", l.c_str());
  CHECK(!Has(l, "{3,4}:100"));   // the right line would say {3,4}:100: the plant must not give it
  CHECK(Has(l, "{3}:150"));      // the wrong key merged both sets
  mh::g_plant = 0;
  Frames(3200, 3300, 1u << 10);
  Frames(3300, 3350, (1u << 4) | (1u << 10));
  l = mh::MissingLine();
  CHECK(Has(l, "{3,4}:100") && Has(l, "{3}:50"));

  std::printf(g_fail ? "x1_hist_unit: %d FAIL\n" : "x1_hist_unit: all ok\n", g_fail);
  return g_fail ? 1 : 0;
}
