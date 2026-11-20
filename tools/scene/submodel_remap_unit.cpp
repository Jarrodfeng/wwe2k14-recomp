// S105 C2LA F1: the sub-model material remap (src\renderer_scene\sub_model_remap.h, called by rf_c_core.cpp SubModel). A full-model material names its textures by indices around 100+;
// the sub-model holds ~12 images, so every texture field must be remapped. The unit builds a "full" model whose textures are named, remaps a material that names a texture in EVERY field
// (noise = mat_noize) and reads each field back through the sub-model's list. Planted twin: --plant skip_noise (the pre-fix gap) must FAIL the noise row.
//   pwsh -NoProfile -File tools\scene\build_submodel_remap_unit.ps1  ->  out\tools\submodel_remap_unit.exe [--plant skip_noise]
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/sub_model_remap.h"

static int g_fail = 0;
static void Check(const char* name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
  const bool plant = argc > 2 && !std::strcmp(argv[1], "--plant") && !std::strcmp(argv[2], "skip_noise");
  std::vector<std::string> full;   // the full model's texture names: 150 images, so indices run past a 12-image sub-model
  for (int i = 0; i < 150; ++i) full.push_back("t" + std::to_string(i));
  full[101] = "mat_noize";
  assets::Material mt;
  mt.diffuse = 100, mt.normal = 102, mt.lightmap = 103, mt.mask = 104, mt.specular = 105, mt.detail = 106, mt.noise = 101;
  mt.layer[0] = 110, mt.layer[1] = 111, mt.layer[2] = 112;
  std::vector<std::string> sub;
  std::map<int, int> of;
  const auto tex = [&](int t) {
    if (t < 0 || size_t(t) >= full.size()) return -1;
    auto it = of.find(t);
    if (it != of.end()) return it->second;
    sub.push_back(full[size_t(t)]);
    return of[t] = int(sub.size()) - 1;
  };
  rdr_scene::RemapMaterialTextures(mt, tex, plant);
  const auto name = [&](int i) { return i >= 0 && size_t(i) < sub.size() ? sub[size_t(i)] : std::string("<none>"); };
  Check("diffuse", name(mt.diffuse) == "t100");
  Check("normal", name(mt.normal) == "t102");
  Check("lightmap", name(mt.lightmap) == "t103");
  Check("mask", name(mt.mask) == "t104");
  Check("specular", name(mt.specular) == "t105");
  Check("detail", name(mt.detail) == "t106");
  Check("noise is mat_noize in the sub-model (the ring mat grain)", name(mt.noise) == "mat_noize");
  Check("layers", name(mt.layer[0]) == "t110" && name(mt.layer[1]) == "t111" && name(mt.layer[2]) == "t112");
  std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
