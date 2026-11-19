// S103 CEF: counts the meshes whose part starts "ring steps" / "announce desk" in arenas loaded by the C++ loader (src\assets\asset_load.h).
//   props_check.exe --game DIR [--plant] ARENA...   -> one line per arena: "arena N steps S desk D total T"
// --plant sets assets::load_detail::AlwaysPropsPlant (the old behaviour: the props on every arena). Build: tools\assets\build_props_check.ps1. Reads game files only.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"

int main(int argc, char** argv) {
  std::string game;
  std::vector<int> ids;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--plant") assets::load_detail::AlwaysPropsPlant() = true;
    else ids.push_back(std::atoi(a.c_str()));
  }
  if (game.empty() || ids.empty()) {
    std::fprintf(stderr, "usage: props_check --game DIR [--plant] ARENA...\n");
    return 2;
  }
  launcher::GameFiles files;
  files.SetRoot(game);
  int rc = 0;
  for (int id : ids) {
    assets::Model m;
    std::string err;
    if (!assets::LoadArena(files, id, m, err)) {
      std::printf("arena %d FAILED %s\n", id, err.c_str());
      rc = 1;
      continue;
    }
    int steps = 0, desk = 0;
    for (const assets::Mesh& me : m.meshes) {
      if (me.part.rfind("ring steps", 0) == 0) ++steps;
      else if (me.part.rfind("announce desk", 0) == 0) ++desk;
    }
    std::printf("arena %d steps %d desk %d total %zu\n", id, steps, desk, m.meshes.size());
  }
  return rc;
}
