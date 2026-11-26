// S111 RIMGAME unit: the skin's rim constants against the game's own rules. 0 game runs.
//   c161 RimLightParam = (1, 0.83, 1, 0): the rim object's constructor constants [C sub_827EA7F0 +1B4..+1CC], copied to G+320 by sub_827E9D90 +64..+98.
//   c162 RimLightDepth = (near, far, z, 0), z = G+308 [C sub_827E9D90 +BC..+E4]: d = far - near; d > 0 ? clamp(d / 1000, 0, 1) : 1 (1000 = obj+72, sub_827EA7F0 +1C4),
//   carried into the model's block + 32 by sub_82BD7B50 +590..+5BC. Without a rim map: (0, 1, 1, 0).
//   out\tools\rimgame_unit.exe   -> PASS / FAIL, exit 0 / 1 (tools\scene\build_rimgame_unit.ps1 builds and runs it)
// The planted twin (the old z = (far - near) * 0.001, no clamp) runs inside and must FAIL.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../src/renderer_scene/char_effects.h"

using namespace rdr_scene;

namespace {
// The game's G+308, written from the listing's branches (not from our resolver): fsubs, then the gt / lt / gt compares and the fdivs.
float GameZ(float near_, float far_) {
  const float d = far_ - near_;
  if (!(d > 0.f)) return 1.f;   // +BC: ble -> f0 keeps 1.0 (0x8218B8C0)
  const float t = d / 1000.f;   // +C0..+C4: fdivs by obj+72
  if (t < 0.f) return 0.f;      // +CC
  if (t > 1.f) return 1.f;      // +DC
  return t;
}
bool Same(const float* a, const float* b) { return std::memcmp(a, b, 16) == 0; }
}  // namespace

int main() {
  assets::Material mat;
  assets::LightSet ls;
  int bad = 0;
  // c161
  {
    CharCtx c;
    c.mat = &mat, c.lights = &ls;
    float got[4] = {9, 9, 9, 9};
    const float want[4] = {1.f, 0.83f, 1.f, 0.f};
    const bool known = ResolveChar("RimLightParam", 1, c, got);
    const bool ok = known && Same(got, want);
    std::printf("%s  c161 RimLightParam = (%g, %g, %g, %g), the game's constants (1, 0.83, 1, 0)\n", ok ? "ok  " : "FAIL", got[0], got[1], got[2], got[3]);
    if (!ok) ++bad;
  }
  // c162: the captured range, a range over 1000, an empty range, a reversed range
  const float cases[][2] = {{597.354f, 776.737f}, {100.f, 1600.f}, {500.f, 500.f}, {700.f, 500.f}, {1250.609f, 1366.783f}};
  const int n = int(sizeof cases / sizeof cases[0]);
  int real_bad = 0, twin_bad = 0;
  for (int i = 0; i < n; ++i) {
    const float params[4] = {512.f, 512.f, cases[i][0], cases[i][1]};
    CharCtx c;
    c.mat = &mat, c.lights = &ls, c.rim_params = params;
    float got[4] = {9, 9, 9, 9};
    const float want[4] = {cases[i][0], cases[i][1], GameZ(cases[i][0], cases[i][1]), 0.f};
    const bool known = ResolveChar("RimLightDepth", 1, c, got);
    if (!known || !Same(got, want)) {
      std::printf("FAIL  c162 near %g far %g: ours (%g, %g, %g, %g), the game's z %g\n", cases[i][0], cases[i][1], got[0], got[1], got[2], got[3], want[2]);
      ++real_bad;
    }
    const float twin[4] = {cases[i][0], cases[i][1], (cases[i][1] - cases[i][0]) * 0.001f, 0.f};
    if (!Same(twin, want)) ++twin_bad;
  }
  {
    CharCtx c;
    c.mat = &mat, c.lights = &ls;
    float got[4] = {9, 9, 9, 9};
    const float want[4] = {0.f, 1.f, 1.f, 0.f};
    if (!ResolveChar("RimLightDepth", 1, c, got) || !Same(got, want)) {
      std::printf("FAIL  c162 without a rim map: (%g, %g, %g, %g), want the neutral (0, 1, 1, 0)\n", got[0], got[1], got[2], got[3]);
      ++real_bad;
    }
  }
  bad += real_bad;
  std::printf("%s  c162 RimLightDepth: %d of %d cases differ from the game's G+308 rule\n", real_bad ? "FAIL" : "ok  ", real_bad, n + 1);
  std::printf("%s  planted twin (the old (far - near) * 0.001, no clamp): %d of %d cases wrong %s\n", twin_bad ? "ok  " : "FAIL", twin_bad, n, twin_bad ? "(caught)" : "(NOT caught)");
  if (!twin_bad) ++bad;
  std::printf("%s rimgame_unit: c161 = the rim object's constants, c162.z = the game's clamp\n", bad ? "FAIL" : "PASS");
  return bad ? 1 : 0;
}
