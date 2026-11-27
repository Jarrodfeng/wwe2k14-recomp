// S105 A7: offline proof that every alias row of a model PACH resolves (OrdinalOfModelKeyIn, src\assets\asset_load.h) and that a texture directory in
// entry 39 is read (AddTexDirs over tex_dir_order.h; the order itself: tools\scene\texdirorder_unit.cpp). 0 game runs; the fixture is a synthetic pac of our own bytes (nothing from the game).
// entry: assets::load_detail::OrdinalOfModelKeyIn (the body of assets::OrdinalOfModelKey, which only opens the pac and calls it)
//   a7_unit selftest [--plant old_first_row|no_entry_39]   the fixture checks; a plant must make a check FAIL (exit 1)
//   a7_unit game GAMEDIR PACKDIR PACKID ID KEY...           read-only: assets::OrdinalOfModelKey through launcher::GameFiles, the mod pack published as an overlay
//   a7_unit real FILE KEY...                              read-only: the model-PACH ordinal each KEY resolves to in a real pac (e.g. the mod's ch397_dlc06.pac)
// Build: pwsh -NoProfile -File tools\scene\build_a7_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <filesystem>

#include "../../src/assets/asset_load.h"
#include "../../src/dlc_overlays.h"
#include "../../src/renderer_scene/wrestler_src.h"

using namespace assets;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
static void PutLe32(Bytes& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }

// A PACH at `at` (2 KB aligned) with the given entries (id, payload).
static void PutPach(Bytes& b, size_t at, const std::vector<std::pair<uint32_t, Bytes>>& ents) {
  const size_t first = at + 8 + 12 * ents.size();
  std::memcpy(b.data() + at, "PACH", 4);
  PutLe32(b, at + 4, uint32_t(ents.size()));
  size_t off = 0;
  for (size_t i = 0; i < ents.size(); ++i) {
    PutLe32(b, at + 8 + 12 * i, ents[i].first);
    PutLe32(b, at + 8 + 12 * i + 4, uint32_t(off));
    PutLe32(b, at + 8 + 12 * i + 8, uint32_t(ents[i].second.size()));
    std::memcpy(b.data() + first + off, ents[i].second.data(), ents[i].second.size());
    off += ents[i].second.size();
  }
}
static void PutRow(Bytes& b, int row, const char* name8, uint32_t sector) {
  const size_t o = 0x80C + 16 * size_t(row);
  std::memcpy(b.data() + o, name8, 8);
  PutLe32(b, o + 8, sector);
  PutLe32(b, o + 12, 0x10);
}
// A texture directory with one texture `name` (the 16-byte name, "DDS\0" tag, size, offset: dds.h TexSet::AddDir).
static Bytes TexDir(const char* name) {
  Bytes d(0x10 + 32 + 8, 0);
  PutLe32(d, 0, 1);
  std::memcpy(d.data() + 0x10, name, std::strlen(name));
  std::memcpy(d.data() + 0x10 + 16, "DDS", 3);
  PutLe32(d, 0x10 + 20, 8);
  PutLe32(d, 0x10 + 24, 0x30);
  return d;
}

static int SelfTest(const std::string& plant) {
  // PACH A (sector 0, ordinal 0): a model (entry 0) + a texture dir in entry 12. PACH B (sector 2, ordinal 1): a model + a texture dir in entry 39.
  Bytes f(0x4000 + 0x800 * 4, 0);
  std::memcpy(f.data(), "EPAC", 4);
  std::memcpy(f.data() + 0x800, "EMD ", 4);
  PutRow(f, 0, "00039702", 0);   // an imported superstar: three alias rows, ONE PACH (importer_core.cpp)
  PutRow(f, 1, "00039712", 0);
  PutRow(f, 2, "00039722", 0);
  PutRow(f, 3, "00039742", 2);   // the second PACH
  PutRow(f, 4, "00039752", 3);   // a row whose sector holds no PACH
  PutRow(f, 5, "00039762", 3);   // a row at a PACH with no model, then a second row of the same name at a model PACH: the scan goes on (review 6)
  PutRow(f, 6, "00039762", 2);
  PutPach(f, 0x4000 + 3 * 0x800, {{12, TexDir("nomodel")}});
  PutPach(f, 0x4000, {{0, Bytes(16, 1)}, {12, TexDir("colorA")}});
  PutPach(f, 0x4000 + 2 * 0x800, {{0, Bytes(16, 2)}, {39, TexDir("colorB")}});
  pac::Pac p;
  std::string err;
  Check(pac::OpenBytes(f, "synthetic", p, err), "the synthetic pac opens (2 PACHs)");
  Check(p.pachs.size() == 3, "three PACHs (the third has no model)");

  // The shipped rule, or (plant old_first_row) the old rule: loop over ordinals and ask the PACH's own (first-row) key.
  auto ordinal = [&](int key) {
    if (plant != "old_first_row") return load_detail::OrdinalOfModelKeyIn(p, key);
    for (int i = 0; i < 2; ++i)
      if (load_detail::ModelKeyIn(p, i) == key) return i;
    return -1;
  };
  Check(ordinal(39702) == 0, "key 39702 (the first row) -> ordinal 0");
  Check(ordinal(39712) == 0, "key 39712 (alias row 2) -> ordinal 0");
  Check(ordinal(39722) == 0, "key 39722 (alias row 3) -> ordinal 0");
  Check(ordinal(39742) == 1, "key 39742 (the second PACH) -> ordinal 1");
  Check(ordinal(39732) == -1, "key 39732 (no row) -> -1");
  Check(ordinal(39762) == 1, "key 39762 (a row at a model-less PACH, then a row at PACH 1) -> ordinal 1");
  Check(ordinal(39752) == -1, "key 39752 (row at a sector with no PACH) -> -1");
  Check(ordinal(-1) == -1, "key -1 -> -1");

  // entry 39: a texture directory the game's factory reads.
  auto has = [&](const pac::Pach& h, const char* tex) {
    TexSet ts;
    if (plant == "no_entry_39") {   // the old filter: 10..19 only
      for (const pac::Ent& e : h.ents)
        if (e.id >= 10 && e.id <= 19 && e.size >= 0x30) ts.AddDir(pac::Decode(p.bytes.data() + e.off, e.size));
    } else {
      load_detail::AddTexDirs(ts, p.bytes, h);
    }
    return ts.Has(tex);
  };
  Check(has(p.pachs[0], "colorA"), "a texture in entry 12 is found");
  Check(has(p.pachs[1], "colorB"), "a texture in entry 39 is found");
  Check(!has(p.pachs[1], "colorA"), "a texture of the other PACH is not found");
  {
    const std::vector<uint32_t>& keys = tex_dir_order::Keys(tex_dir_order::Owner::kCharacter);
    Check(std::find(keys.begin(), keys.end(), 39u) != keys.end() && std::find(keys.begin(), keys.end(), 20u) == keys.end() && std::find(keys.begin(), keys.end(), 9u) == keys.end(),
          "a character's directories include 39, not 9 or 20");
  }
  std::printf("%s (plant: %s)\n", g_fail ? "FAIL" : "ALL PASS", plant.empty() ? "none" : plant.c_str());
  return g_fail ? 1 : 0;
}

static int Real(const char* path, int nkeys, char** keys) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::printf("cannot open %s\n", path), 2;
  Bytes raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  pac::Pac p;
  std::string err;
  if (!pac::OpenBytes(std::move(raw), path, p, err)) return std::printf("not a pac: %s\n", err.c_str()), 2;
  int models = 0;
  for (const pac::Pach& h : p.pachs) models += pac::Find(h, 0) ? 1 : 0;
  std::printf("%s: %zu PACH, %d with a model\n", path, p.pachs.size(), models);
  for (int i = 0; i < nkeys; ++i) {
    const int key = std::atoi(keys[i]);
    std::printf("key %d: new rule (any row) -> ordinal %d", key, load_detail::OrdinalOfModelKeyIn(p, key));
    int old = -1;
    for (int a = 0; a < models; ++a)
      if (load_detail::ModelKeyIn(p, a) == key) { old = a; break; }
    std::printf("; old rule (first row of each PACH) -> %d\n", old);
  }
  return 0;
}

// assets::OrdinalOfModelKey (the production entry) through launcher::GameFiles with the mod pack published as an overlay, as dlc.h publishes it.
static int Game(const char* game, const char* packdir, int packid, int id, int nkeys, char** keys) {
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  char sfx[16];
  std::snprintf(sfx, sizeof sfx, "_dlc%02d", packid);
  wwe2k14::dlc_overlays::Publish({{std::filesystem::path(packdir), sfx, "mod", packid}});
  uint64_t gen = ~0ull;
  rdr_scene::wrestler_src::ApplyOverlays(files, &gen);
  for (int i = 0; i < nkeys; ++i) std::printf("assets::OrdinalOfModelKey(%d, %s) = %d\n", id, keys[i], assets::OrdinalOfModelKey(files, id, std::atoi(keys[i])));
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "selftest") == 0)
    return SelfTest(argc >= 4 && std::strcmp(argv[2], "--plant") == 0 ? argv[3] : "");
  if (argc >= 7 && std::strcmp(argv[1], "game") == 0) return Game(argv[2], argv[3], std::atoi(argv[4]), std::atoi(argv[5]), argc - 6, argv + 6);
  if (argc >= 4 && std::strcmp(argv[1], "real") == 0) return Real(argv[2], argc - 3, argv + 3);
  std::printf("usage: a7_unit selftest [--plant old_first_row|no_entry_39] | real FILE KEY...\n");
  return 2;
}
