// Session 101 lane 275: prints the arena viewer's ring placements (assets::PlaceRing, src\assets\asset_load.h) in the GAME's file space, as 3x4 column-form rows
// (a b c d per row: x' = a x + b y + c z + d), one line per placed copy, so tools\ring\ring_place_check.py can compare them with a capture's ring-rows records.
//   ring_place_dump.exe          -> lines "label copy r0(4) r1(4) r2(4)"; no game files are read (the pieces are 1-mesh stubs)
// Build: tools\assets\build_ring_place.ps1 (out\tools\ring_place_dump.exe).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <map>

#include "../../src/assets/asset_load.h"

int main() {
  using namespace assets;
  Model m;
  std::map<uint32_t, std::pair<size_t, size_t>> ring;
  size_t at = 0;
  for (uint32_t key : {951u, 952u, 953u, 960u, 964u, 973u, 974u}) {
    m.meshes.emplace_back();
    ring[key] = {at, at + 1};
    ++at;
  }
  load_detail::PlaceRing(ring, m);
  for (const Placement& p : m.placements) {
    int copy = 0;
    for (const Matrix& v : p.transforms) {
      const Matrix f = MirrorY(v);   // back to file space (MirrorY is its own inverse); row-vector: p' = p * f
      std::printf("%s %d", p.label.c_str(), copy++);
      for (int row = 0; row < 3; ++row) std::printf(" %.4f %.4f %.4f %.4f", f[0 * 4 + row], f[1 * 4 + row], f[2 * 4 + row], f[3 * 4 + row]);
      std::printf("\n");
    }
  }
  return 0;
}
