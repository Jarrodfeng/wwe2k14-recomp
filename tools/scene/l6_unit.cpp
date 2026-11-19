// S103 L6 unit: the shadow reader's constants come from the MAP (MapImage::prm), nothing hard-coded. 0 game runs.
//   out\tools\l6_unit.exe            -> PASS / FAIL, exit 0 / 1
//   out\tools\l6_unit.exe --plant no_prm   (the old behaviour: no prm handed to the resolver; ShadowPrm stays neutral zero) must FAIL
#include <cstdio>
#include <cstring>

#include "../../src/renderer_scene/char_effects.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  const bool plant = argc > 2 && !std::strcmp(argv[1], "--plant") && !std::strcmp(argv[2], "no_prm");
  assets::Material mat;
  assets::LightSet ls;
  float matrix[16], params[4] = {512, 512, 59771.f, 60048.f};
  for (int i = 0; i < 16; ++i) matrix[i] = float(i + 1);
  float prmA[12], prmB[12];
  for (int i = 0; i < 12; ++i) prmA[i] = 0.5f + float(i), prmB[i] = prmA[i];
  prmB[3] = 0.9f;    // row 0 w: the strength (was hard-coded 0.7)
  prmB[10] = 33.f;   // row 2 z
  const auto resolve = [&](const float* prm, const char* name, uint32_t vec4s, float* out) {
    CharCtx c;
    c.mat = &mat, c.lights = &ls, c.shadow_matrix = matrix, c.shadow_params = params, c.shadow_prm = plant ? nullptr : prm;
    return ResolveChar(name, vec4s, c, out);
  };
  float a[12] = {}, b[12] = {}, xy[8] = {}, w[4] = {};
  resolve(prmA, "ShadowPrm", 3, a);
  resolve(prmB, "ShadowPrm", 3, b);
  resolve(prmA, "TexProjLightXY", 2, xy);
  resolve(prmA, "TexProjLightW", 1, w);
  int bad = 0;
  if (std::memcmp(a, prmA, sizeof a) != 0) std::printf("FAIL  ShadowPrm rows 0-2 are not the map's prm\n"), ++bad;
  if (std::memcmp(b, prmB, sizeof b) != 0 || std::memcmp(a, b, sizeof a) == 0) std::printf("FAIL  a map whose prm differs does not change the bound constants (row 0 w %g vs %g)\n", a[3], b[3]), ++bad;
  if (std::memcmp(xy, matrix, 32) != 0 || std::memcmp(w, matrix + 12, 16) != 0) std::printf("FAIL  TexProjLight rows are not the map's matrix\n"), ++bad;
  std::printf("%s l6_unit: ShadowPrm from MapImage::prm (3 rows), twin map changes row 0 w / row 2 z, TexProjLight rows from the matrix\n", bad ? "FAIL" : "PASS");
  if (plant) { std::printf("%s: the planted twin (no prm) %s\n", bad ? "PASS" : "FAIL", bad ? "was caught" : "was NOT caught"); return bad ? 0 : 1; }
  return bad ? 1 : 0;
}
