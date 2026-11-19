// S103 B3: the REAL arena table parser (src/assets/arena_lm.h, the code the loader and the rig run) over the decoded leaf p0/102/5 of the player's misc02_ingame.pac (b3_lm_check.py dumps it
// under out\ for this run; nothing is committed).
//   b3_unit.exe LEAF.bin [--plant shift|swap|gain01] ID...
// prints one line per id: `ID x y z w` as hex float bits of c184 = (1 / X, 1 / Z, G * 0.01f, 1), then `clamped N no_table M zero Z`. Exit 1 when the leaf is not the layout.
// --plant: shift = the record one index off; swap = X and Z exchanged; gain01 = G * 0.1; (the check must see each one differ from the captured c184). Compile only, no game.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/assets/arena_lm.h"

namespace al = assets::arena_lm;
static uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

int main(int argc, char** argv) {
  if (argc < 2) return std::printf("usage: b3_unit LEAF.bin [--plant NAME] ID...\n"), 2;
  std::vector<uint8_t> leaf;
  if (FILE* f = std::fopen(argv[1], "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) leaf.insert(leaf.end(), buf, buf + n);
    std::fclose(f);
  }
  al::Table t;
  if (!al::ParseLeaf(leaf.data(), leaf.size(), t)) return std::printf("FAIL: %s is not the arena post table layout\n", argv[1]), 1;
  std::string plant;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) { plant = argv[++i]; continue; }
    const int id = std::atoi(argv[i]);
    al::Param p = al::Compute(&t, plant == "shift" ? id + 1 : id);
    if (plant == "swap") std::swap(p.v[0], p.v[1]);
    if (plant == "gain01") {
      const int rid = al::RemapId(id);
      if (rid >= 0 && size_t(rid) < t.rec.size()) p.v[2] = float(double(t.rec[size_t(rid)].g) * 0.1);   // (the plant: x 0.1 instead of x 0.01f)
    }
    std::printf("%d %08X %08X %08X %08X\n", id, Bits(p.v[0]), Bits(p.v[1]), Bits(p.v[2]), Bits(p.v[3]));
  }
  std::printf("clamped %llu no_table %llu zero %llu records %zu\n", (unsigned long long)al::g_clamped.load(), (unsigned long long)al::g_no_table.load(), (unsigned long long)al::g_zero.load(), t.rec.size());
  return 0;
}
