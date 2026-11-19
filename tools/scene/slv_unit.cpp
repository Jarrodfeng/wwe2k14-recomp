// S103 lane SLV / SLV2: unit test of the blank plane rule (src/renderer_scene/plane_blank_rule.h). No GPU, no game. Built and run by slv_check.py (the same file is compiled against a mutated copy of the header
// for each planted twin: the unit must exit 1 on every one).
//   slv_unit            exit 0 = every case behaved, 1 = a case failed
// The rule: the ground square's four corners all at clip w <= 0 under M = fit x camera (row 3 of M holds w). Weight 0 (module+104) is NOT the rule: it is also 0 when the square fills the screen height, a real shadow.
#include <cmath>
#include <cstdio>
#include <limits>

#include "plane_blank_rule.h"

using rdr_scene::PlaneChainBlank;
using rdr_scene::PlaneSquareCornersBehind;

static int g_bad = 0;
static void Expect(bool got, bool want, const char* what) {
  std::printf("%s %s\n", got == want ? "ok  " : "FAIL", what);
  if (got != want) ++g_bad;
}
static void ExpectInt(int got, int want, const char* what) {
  std::printf("%s %s (%d)\n", got == want ? "ok  " : "FAIL", what, got);
  if (got != want) ++g_bad;
}

// M with row 3 = (a, b, c, d): the clip w of the point (x, y, z, 1) is a x + b y + c z + d. Rows 0-2 are arbitrary (the rule does not read them).
static void Matrix(float* m, float a, float b, float c, float d) {
  for (int i = 0; i < 16; ++i) m[i] = i % 5 == 0 ? 1.f : 0.f;
  m[12] = a, m[13] = b, m[14] = c, m[15] = d;
}

int main() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  float m[16];
  Matrix(m, 0, 0, 0, 1);
  ExpectInt(PlaneSquareCornersBehind(m, -120.f), 0, "w = 1 everywhere: no corner behind");
  Expect(PlaneChainBlank(m, -120.f), false, "a real shadow (every corner in front) is never blank, whatever the weight module+104 holds");
  Matrix(m, 0, 0, 0, 0);
  ExpectInt(PlaneSquareCornersBehind(m, -120.f), 4, "w = 0 at every corner (the sentinel fit): four behind");
  Expect(PlaneChainBlank(m, -120.f), true, "the sentinel fit is blank");
  Matrix(m, 0, 0, 0, -1);
  Expect(PlaneChainBlank(m, -120.f), true, "w = -1 at every corner is blank");
  Matrix(m, 1, 0, 1, 700);   // w = x + z + 700 over (+-640, +-640): -580, 700, 1980, 700
  ExpectInt(PlaneSquareCornersBehind(m, 0.f), 1, "one corner behind the camera (the game draws it, clipped)");
  Expect(PlaneChainBlank(m, 0.f), false, "one corner behind is NOT blank (the game's own case, the hardware clips it)");
  Matrix(m, 1, 0, 1, 0);   // w = x + z: -1280, 0, 1280, 0
  ExpectInt(PlaneSquareCornersBehind(m, 0.f), 3, "three corners at or behind (w = 0 counts as behind)");
  Expect(PlaneChainBlank(m, 0.f), false, "three behind is NOT blank");
  Matrix(m, 0, 1, 0, -50);   // w = y - 50 with y = -120: -170 everywhere; y = 60: 10 everywhere
  Expect(PlaneChainBlank(m, -120.f), true, "the plane height enters: y = -120 puts every corner behind");
  Expect(PlaneChainBlank(m, 60.f), false, "the plane height enters: y = 60 puts every corner in front");
  Matrix(m, nan, 0, 0, 1);
  Expect(PlaneChainBlank(m, -120.f), true, "a NaN w never counts as in front (the host refuses a non-finite fit before this)");
  std::printf("%s\n", g_bad ? "slv_unit: FAIL" : "slv_unit: PASS");
  return g_bad ? 1 : 0;
}
