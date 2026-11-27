// S111 VTXEXTENT: OUR loaded model's vertex extent per mesh, for tools\scene\vtxextent.py (offline, 0 game runs, header-only build like tools\assets\splatscale_unit.cpp).
// The loaders run exactly as the game exe's GameFilesSource runs them for a bg pac: assets::LoadArena (stage slots 0..399, ring and screen keys: every mesh keeps src_key / src_record) and
// assets::LoadBloodEntry (key 32000, one JBOY). Other keys (31000 crowd flash, 5000 ...) have no header-only loader: the python side says "no ours".
//   vtxextent_ours.exe --game DIR --arena N        one line per mesh:  M <arena> <key> <record> <vertices> <lox> <loy> <loz> <hix> <hiy> <hiz>   (viewer space: FinishModel mirrors y; compare WIDTHS)
//                                                  a loader that fails prints:  E <arena> <what> <error>
// Build: pwsh -NoProfile -File tools\scene\build_vtxextent_ours.ps1 [-Root DIR] (Root = the checkout whose src\assets is compiled, e.g. a branch worktree).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "asset_load.h"

namespace {

void PrintMeshes(int arena, const assets::Model& m, bool blood) {
  for (const assets::Mesh& me : m.meshes) {
    if (me.vertices.empty()) continue;
    if (!blood && me.src_kind != 1) continue;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const assets::Vertex& v : me.vertices)
      for (int k = 0; k < 3; ++k) lo[k] = std::fmin(lo[k], v.pos[k]), hi[k] = std::fmax(hi[k], v.pos[k]);
    std::printf("M %d %d %d %zu %.6f %.6f %.6f %.6f %.6f %.6f\n", arena, blood ? 32000 : me.src_key, me.src_record, me.vertices.size(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string game;
  int arena = -1;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
    else if (!std::strcmp(argv[i], "--arena") && i + 1 < argc) arena = std::atoi(argv[++i]);
  }
  if (game.empty() || arena < 0) { std::fprintf(stderr, "usage: vtxextent_ours --game DIR --arena N\n"); return 2; }
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(game));
  {
    assets::Model m;
    std::string err;
    if (assets::LoadBloodEntry(files, arena, 0, m, err)) PrintMeshes(arena, m, true);
    else std::printf("E %d blood %s\n", arena, err.c_str());
  }
  {
    // the settings GameFilesSource::Load (live_assets.cpp) sets around LoadArena in the game exe: both looks' slots, the low ring keys 980-982, every stage screen key
    assets::load_detail::RingLowKeys() = true;
    assets::load_detail::KeepLookSlots() = true;
    assets::stage_media::Cfg().all_keys = true;
    assets::Model m;
    std::string err;
    if (assets::LoadArena(files, arena, m, err)) PrintMeshes(arena, m, false);
    else std::printf("E %d arena %s\n", arena, err.c_str());
  }
  return 0;
}
