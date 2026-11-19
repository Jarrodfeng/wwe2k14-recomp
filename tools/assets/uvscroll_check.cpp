// S103 lane E6: reads the UV-scroll speeds (assets::Material::scroll, jboy.h) of an arena's scrolling materials from the player's own game files.
//   uvscroll_check.exe --game DIR [--arena N | --all] [--list OUT.txt] [--plant wrong_slot|wrong_layer|no_read]
// Prints one line per UVScroll mesh (arena, slot, record, shader, UVScroll0 = U V U2 V2, UVScroll1 = U3 V3 U4 V4, have) and a summary
//   [uvscroll arena N meshes M materials K have H absent A nonzero Z]
// Exit 0 when at least one UVScroll material has a non-zero speed (the proof that the speeds are read); 1 otherwise. --list writes the distinct
// (UVScroll0, UVScroll1) pairs, one per line, for tools\assets\uvscroll_vs_stream.py (the comparison with a capture's VS c203 / c204).
// --plant no_read: the twin "the loader's fields are ignored": the check must FAIL (exit 0 here = it did). The wrong-slot / wrong-layer twins are in
// uvscroll_vs_stream.py (they need the stream to be told apart).
// Reads game files only; writes only the optional --list (keep it under out\).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"

using namespace assets;

int main(int argc, char** argv) {
  std::string game, list, plant;
  int arena = -1;
  bool all = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--game") game = next();
    else if (a == "--arena") arena = std::atoi(next().c_str());
    else if (a == "--all") all = true;
    else if (a == "--list") list = next();
    else if (a == "--plant") plant = next();
    else return std::fprintf(stderr, "usage: uvscroll_check --game DIR [--arena N | --all] [--list OUT] [--plant NAME]\n"), 2;
  }
  if (game.empty() || (arena < 0 && !all)) return std::fprintf(stderr, "--game DIR and --arena N or --all are required\n"), 2;
  {  // a relative --game from a worktree (.claude\worktrees\NAME) is the main checkout's game_data (the same fallback as tools\assets\build.ps1)
    std::error_code ec;
    if (!std::filesystem::exists(game, ec) && std::filesystem::exists("../../../" + game, ec)) game = "../../../" + game;
  }
  launcher::GameFiles files;
  files.SetRoot(game);
  std::vector<int> ids;
  if (all) ids = ListArenaIds(files);
  else ids.push_back(arena);
  std::set<std::string> pairs;
  size_t total_nonzero = 0, total_mats = 0, total_absent = 0;
  for (int id : ids) {
    Model m;
    std::string err;
    if (!LoadArena(files, id, m, err)) {
      std::printf("arena %d: load failed: %s\n", id, err.c_str());
      continue;
    }
    size_t meshes = 0, have = 0, absent = 0, nonzero = 0;
    std::set<int> seen;
    for (const Mesh& me : m.meshes) {
      if (me.material < 0 || size_t(me.material) >= m.materials.size()) continue;
      const Material& mt = m.materials[size_t(me.material)];
      if (mt.shader.find("UVScroll") == std::string::npos && mt.shader.find("UVScrol") == std::string::npos) continue;
      ++meshes;
      float s[8];
      for (int k = 0; k < 8; ++k) s[k] = mt.scroll[k];
      bool h = mt.scroll_have;
      if (plant == "no_read") std::memset(s, 0, sizeof s), h = false;
      if (h) ++have; else ++absent;
      bool nz = false;
      for (int k = 0; k < 8; ++k) nz = nz || s[k] != 0.f;
      if (nz) ++nonzero;
      char line[256];
      std::snprintf(line, sizeof line, "%g %g %g %g | %g %g %g %g", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
      if (h) pairs.insert(line);
      if (seen.insert(me.material).second)
        std::printf("arena %d  %-10s rec %-3d %-16s UVScroll0 %s  have %d\n", id, me.part.c_str(), me.src_record, mt.shader.c_str(), line, h ? 1 : 0);
    }
    std::printf("[uvscroll arena %d meshes %zu have %zu absent %zu nonzero %zu]\n", id, meshes, have, absent, nonzero);
    total_mats += meshes, total_nonzero += nonzero, total_absent += absent;
  }
  if (!list.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(list).parent_path(), ec);
    std::FILE* f = std::fopen(list.c_str(), "w");
    if (!f) return std::printf("cannot write %s\n", list.c_str()), 1;
    for (const std::string& p : pairs) std::fprintf(f, "%s\n", p.c_str());
    std::fclose(f);
  }
  std::printf("[uvscroll total meshes %zu absent %zu nonzero %zu distinct %zu]\n", total_mats, total_absent, total_nonzero, pairs.size());
  const bool planted = !plant.empty();  // a plant passes (exit 0) only when the check caught it
  if (!total_mats) return std::printf("FAIL no UVScroll mesh found\n"), 1;
  if (!total_nonzero) return std::printf(planted ? "caught: UVSCROLL READ (every UVScroll material reads 0)\n" : "FAIL UVSCROLL READ: every UVScroll material reads 0\n"), planted ? 0 : 1;
  if (planted) return std::printf("FAIL plant %s was not caught\n", plant.c_str()), 1;
  std::printf("PASS\n");
  return 0;
}
