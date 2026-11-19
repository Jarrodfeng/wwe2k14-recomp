// S103 CPF: loads Cena's cap prop (evtobj.pac PACH 181 / 182, texture 13904Cap, a DXT5 gloss mask: alpha 0..40) through the shipped loader and checks that no material using it is
// alpha-tested or cut-out-blended (it was drawn at ~2 % opacity). Reads the player's own game files only (read-only).
//   cap_check --game DIR [--plant]
// --plant sets assets::load_detail::CpfPlant() (the old rule; needs a -DRDR_DEV build) and exits 0 only when the check then FAILS (the plant is caught). Exit 1 = a failure, 2 = usage / cannot read.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "../../src/assets/asset_load.h"

using namespace assets;

int main(int argc, char** argv) {
  std::string game;
  bool plant = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--plant") plant = true;
    else return std::fprintf(stderr, "usage: cap_check --game DIR [--plant]\n"), 2;
  }
  if (game.empty()) return std::fprintf(stderr, "--game DIR is required\n"), 2;
  {
    std::error_code ec;
    if (!std::filesystem::exists(game, ec) && std::filesystem::exists("../../../" + game, ec)) game = "../../../" + game;
  }
  if (plant) assets::load_detail::CpfPlant() = true;   // S103 LNT: the flag, not an env var (jboy.h CPFPlantOld)
  launcher::GameFiles files;
  files.SetRoot(game);
  int cap_mats = 0, bad = 0;
  for (int pach : {181, 182}) {
    for (uint32_t k = 0; k < 10; ++k) {
      Model m;
      std::string err;
      if (!LoadPropEntry(files, pach, k, m, err)) continue;
      for (const Mesh& me : m.meshes) {
        if (me.material < 0 || size_t(me.material) >= m.materials.size()) continue;
        const Material& mt = m.materials[size_t(me.material)];
        if (mt.diffuse < 0 || size_t(mt.diffuse) >= m.textures.size() || m.textures[size_t(mt.diffuse)].name != "13904Cap") continue;
        ++cap_mats;
        const bool b = mt.alpha_test || mt.cutout_blend || mt.alpha_blend;
        bad += b;
        std::printf("PACH %d entry %u  %-18s alpha_test %d alpha_blend %d cutout_blend %d  %s\n", pach, k, mt.shader.c_str(), mt.alpha_test, mt.alpha_blend, int(mt.cutout_blend), b ? "BAD" : "ok");
      }
    }
  }
  if (!cap_mats) return std::printf("FAIL no 13904Cap material found (cannot read the player's evtobj.pac?)\n"), 2;
  const bool ok = bad == 0;
  std::printf("cap materials %d, drawn see-through %d\n", cap_mats, bad);
  if (plant) return std::printf(ok ? "FAIL plant not caught\n" : "caught: CAP SEE-THROUGH (the old rule alpha-tests the cap)\n"), ok ? 1 : 0;
  std::printf(ok ? "PASS\n" : "FAIL CAP SEE-THROUGH\n");
  return ok ? 0 : 1;
}
