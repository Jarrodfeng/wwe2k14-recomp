// S103 EYA unit: the eyeball's ambient scale. ResolveChar("Param1") = PS c247 = (g_fFurLevel, 0, g_fDifAmbLevel, 0) from the material
// [C: sub_82BAAEE8 0x82BAD37C id 3 -> x, 0x82BAD3B4 id 5 -> z]. 0 game runs.
//   out\tools\eya_unit.exe            -> PASS / FAIL, exit 0 / 1
//   out\tools\eya_unit.exe --plant    (EyaOldParam1Plant set: the old zero list) must FAIL inside, so the plant run prints PASS only when it was caught
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../src/assets/load_plants.h"
#include "../../src/renderer_scene/char_effects.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  const bool plant = argc > 1 && !std::strcmp(argv[1], "--plant");
  if (plant) assets::load_detail::EyaOldParam1Plant() = true;   // before the first resolve: the plant switch is read once
  assets::LightSet ls;
  const auto resolve = [&](float fur, float dif, float* out) {
    assets::Material mat;
    mat.fur_level = fur, mat.dif_amb_level = dif;
    CharCtx c;
    c.mat = &mat, c.lights = &ls;
    return ResolveChar("Param1", 1, c, out);
  };
  float a[4] = {9, 9, 9, 9}, b[4] = {9, 9, 9, 9}, d[4] = {9, 9, 9, 9};
  const bool ka = resolve(0.25f, 0.8f, a), kb = resolve(0.f, 0.3f, b);
  assets::Material dflt;
  CharCtx c;
  c.mat = &dflt, c.lights = &ls;
  ResolveChar("Param1", 1, c, d);
  int bad = 0;
  if (!ka || !kb) std::printf("FAIL  Param1 is not a known name\n"), ++bad;
  if (a[0] != 0.25f || a[2] != 0.8f) std::printf("FAIL  g_fFurLevel 0.25 / g_fDifAmbLevel 0.8 gave x %g z %g\n", a[0], a[2]), ++bad;
  if (b[2] != 0.3f || a[2] == b[2]) std::printf("FAIL  a material with another g_fDifAmbLevel does not change c247.z (%g vs %g)\n", a[2], b[2]), ++bad;
  if (a[1] != 0.f || a[3] != 0.f) std::printf("FAIL  c247.y / .w must stay 0 [U]: %g %g\n", a[1], a[3]), ++bad;
  if (d[0] != 0.f || d[2] != 0.f) std::printf("FAIL  a material without the parameters must give 0 [L]\n"), ++bad;
  std::printf("%s eya_unit: Param1 = (g_fFurLevel, 0, g_fDifAmbLevel, 0) per material\n", bad ? "FAIL" : "PASS");
  if (plant) { std::printf("%s: the planted twin (zero list kept) %s\n", bad ? "PASS" : "FAIL", bad ? "was caught" : "was NOT caught"); return bad ? 0 : 1; }
  return bad ? 1 : 0;
}
