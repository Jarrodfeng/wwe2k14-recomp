// S111 UNIFY2 (docs\research\s111\UNIFY2.md): the stock materials' texture slots through the production loader (LoadWrestler -> LoadJboy), one line per material, for a before / after compare.
// tools\scene\build_material_samplers_check.ps1 builds this file twice: against the commit before the lane and against the tree. It includes only what both trees have (asset_load.h).
// Read-only, the player's files. 0 game runs.
//   material_slots_dump.exe --game DIR [ID ...]   (default 100 103 107 161 177; every attire, up to 16)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/asset_load.h"

int main(int argc, char** argv) {
  const char* game = nullptr;
  std::vector<int> ids;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else ids.push_back(std::atoi(argv[i]));
  }
  if (!game) {
    std::printf("usage: material_slots_dump --game DIR [ID ...]\n");
    return 2;
  }
  if (ids.empty()) ids = {100, 103, 107, 161, 177};
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  size_t models = 0, mats = 0;
  for (int id : ids) {
    const int n = assets::AttireCount(files, id);
    for (int a = 0; a < n && a < 16; ++a) {
      assets::Model m;
      std::string err;
      if (!assets::LoadWrestler(files, id, a, m, err)) {
        std::printf("ch%d a%d: not loaded (%s)\n", id, a, err.c_str());
        continue;
      }
      ++models;
      const auto tn = [&](int ti) { return ti < 0 ? std::string("-") : size_t(ti) < m.textures.size() ? m.textures[size_t(ti)].name : "#" + std::to_string(ti); };
      for (size_t k = 0; k < m.materials.size(); ++k) {
        const assets::Material& t = m.materials[k];
        ++mats;
        std::printf("ch%d a%d m%zu %s | d %s n %s mk %s lm %s sp %s cu %s rf %s y %s %s %s z %s dt %s sw %s mu %s hn %s hs %s ec %s wk %s | dn %s cn %s lms %d lp %d spp %d\n", id, a, k, t.shader.c_str(),
                    tn(t.diffuse).c_str(), tn(t.normal).c_str(), tn(t.mask).c_str(), tn(t.lightmap).c_str(), tn(t.specular).c_str(), tn(t.cube).c_str(), tn(t.refmap).c_str(), tn(t.layer[0]).c_str(),
                    tn(t.layer[1]).c_str(), tn(t.layer[2]).c_str(), tn(t.noise).c_str(), tn(t.detail).c_str(), tn(t.sweat_bump).c_str(), tn(t.muscle).c_str(), tn(t.hair_noise_map).c_str(),
                    tn(t.hair_shift_map).c_str(), tn(t.env_cube).c_str(), tn(t.wrinkle_normal).c_str(), t.diffuse_name.c_str(), t.cube_name.c_str(), int(t.has_lm_slot), int(t.lm_stage_page), int(t.spec_pow_prm));
      }
    }
  }
  std::printf("dumped %zu models, %zu materials\n", models, mats);
  return models ? 0 : 1;
}
