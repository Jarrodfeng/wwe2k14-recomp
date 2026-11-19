// card_atlas_unit (S103 CB1): the pure rule of src\renderer_scene\card_atlas_rule.h. Build + run: py -I tools\scene\card_atlas_check.py --unit
// Proves: tiles do not overlap and the gutters sit between them; the far-card quad's v maps the per-type picture's rows onto the tile's rows exactly (v = 0 -> the tile's first row edge, v = 1 -> its last);
// the aniso rule; the key mix reacts to every byte and to the length. Planted twins: --plant pitch (pitch without the gutters: tiles touch), --plant v (v mapped without the tile offset).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/card_atlas_rule.h"

using namespace rdr_scene;

static int g_bad = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  g_bad += ok ? 0 : 1;
}

int main(int argc, char** argv) {
  const std::string plant = argc > 2 && std::string(argv[1]) == "--plant" ? argv[2] : "";
  const uint32_t pitch = plant == "pitch" ? kAtlasTile : kAtlasPitch;   // planted: no gutters
  // tiles: [y0, y0 + 64) each; gutter rows y0 - 1 and y0 + 64 are never another tile's rows
  bool disjoint = true, in_bounds = true;
  for (uint32_t t = 0; t < kAtlasTypes; ++t) {
    const uint32_t y0 = t * pitch + kAtlasGutter;
    in_bounds = in_bounds && y0 >= 1 && y0 + kAtlasTile + 1 <= kAtlasHeight;
    if (t + 1 < kAtlasTypes) disjoint = disjoint && (y0 + kAtlasTile) < ((t + 1) * pitch + kAtlasGutter - 1);   // the bottom gutter row of tile t (y0 + 64) is above the top gutter row of tile t + 1 ((t + 1) * pitch)
  }
  Check(disjoint, "each tile has its own gutter row above and below (no row shared with a neighbour)");
  Check(in_bounds, "every tile and both its gutter rows are inside the atlas");
  Check(kAtlasWidth == kAtlasTile, "the atlas is exactly one tile wide (u clamps as before)");
  // v mapping: the sampler's texel centres of the per-type picture (row r centre = (r + 0.5) / 64) land on the tile's atlas row centres
  bool v_ok = true;
  for (uint32_t t = 0; t < kAtlasTypes; ++t)
    for (uint32_t r = 0; r < kAtlasTile; ++r) {
      const float v = (float(r) + 0.5f) / float(kAtlasTile);
      const float av = plant == "v" ? v : AtlasV(t, v);
      const float want = (float(AtlasTileY(t) + r) + 0.5f) / float(kAtlasHeight);
      v_ok = v_ok && std::fabs(av - want) < 1e-6f;
    }
  Check(v_ok, "every texel centre of a picture maps to the same texel centre of its tile");
  Check(AtlasV(0, 0.f) == float(kAtlasGutter) / float(kAtlasHeight) && AtlasV(kAtlasTypes - 1, 1.f) == float(kAtlasHeight - kAtlasGutter) / float(kAtlasHeight), "v = 0 / v = 1 are the first / last tile rows' outer edges");
  Check(CardAtlasUsable(0) && CardAtlasUsable(1) && !CardAtlasUsable(2) && !CardAtlasUsable(16), "the atlas is used at 1x anisotropy only");
  // key mix: one flipped byte and a different length both change it
  unsigned char buf[40];
  for (int i = 0; i < 40; ++i) buf[i] = (unsigned char)(i * 7 + 1);
  const uint64_t base = AtlasMix(1469598103934665603ull, buf, 40);
  bool all = true;
  for (int i = 0; i < 40; ++i) {
    buf[i] ^= 0x10;
    all = all && AtlasMix(1469598103934665603ull, buf, 40) != base;
    buf[i] ^= 0x10;
  }
  Check(all, "the input key changes with any one byte of the input");
  Check(AtlasMix(1469598103934665603ull, buf, 39) != base && AtlasMix(1469598103934665603ull, buf, 40) == base, "the input key depends on the length and is deterministic");
  std::printf("%s (%d failed)\n", g_bad ? "FAIL" : "PASS", g_bad);
  return g_bad ? 1 : 0;
}
