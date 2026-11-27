// S111 TEXDIRORDER: offline proof of (1) the texture directory order of a model PACH (src\assets\tex_dir_order.h, through assets::load_detail::AddTexDirs) and
// (2) a title belt's twin by the game's table 103 (src\assets\belt_twin.h; assets::BeltTwin on a synthetic game folder through launcher::GameFiles).
// S111 MIPLEVELS review: (1) also checks the level count each directory carries (TexSet::AddDirLevels -> Get / Peek: a character's count, an event prop's 3 (S111 PROPLEVELS:
// MakeWeapon's lists; no entry-100 table in this fixture), the blood list's 3).
// S111 LEVELSBYTE: a character's count is the load's (AddTexDirs' char_levels): the stock 4 by default, a handed 1 reaches every character directory and no prop's.
// 0 game runs; every fixture is synthetic bytes of our own (nothing from the game).
//   texdirorder_unit selftest [--plant id_order|belt_ranges]   a plant must make a check FAIL (exit 1)
//     id_order     the old directory rule: every entry 10..19 and 39, in the PACH's stored order, for any owner
//     belt_ranges  the old twin rule: fixed PACH runs (349..398 left, 399..450 right, 451..502 waist), whatever the pac's layout
// Build: pwsh -NoProfile -File tools\scene\build_texdirorder_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../../src/assets/asset_load.h"

using namespace assets;
namespace fs = std::filesystem;

static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
static void PutLe32(Bytes& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }

// A one-mip DXT1 DDS of w x w texels: the width tells which directory a name was taken from.
static Bytes Dds(uint32_t w) {
  Bytes d(128 + size_t((w + 3) / 4) * ((w + 3) / 4) * 8, 0);
  std::memcpy(d.data(), "DDS ", 4);
  PutLe32(d, 4, 124);
  PutLe32(d, 12, w);
  PutLe32(d, 16, w);
  PutLe32(d, 84, 0x31545844u);   // DXT1
  return d;
}
// A texture directory (dds.h TexSet::AddDir's layout): count, 12 header bytes, count x {name[16], "dds", size, offset}, then the DDS blobs.
static Bytes TexDir(const std::vector<std::pair<const char*, uint32_t>>& tex) {
  Bytes d(0x10 + 32 * tex.size(), 0);
  PutLe32(d, 0, uint32_t(tex.size()));
  for (size_t i = 0; i < tex.size(); ++i) {
    const Bytes blob = Dds(tex[i].second);
    const size_t r = 0x10 + 32 * i;
    std::memcpy(d.data() + r, tex[i].first, std::strlen(tex[i].first));
    std::memcpy(d.data() + r + 16, "dds", 3);
    PutLe32(d, r + 20, uint32_t(blob.size()));
    PutLe32(d, r + 24, uint32_t(d.size()));
    d.insert(d.end(), blob.begin(), blob.end());
  }
  return d;
}
// One PACH's entries laid out in a buffer (pac::Pach offsets into it).
struct Fixture {
  Bytes buf;
  pac::Pach h;
};
static Fixture MakePach(const std::vector<std::pair<uint32_t, Bytes>>& ents) {
  Fixture f;
  for (const auto& [id, b] : ents) {
    pac::Ent e;
    e.id = id, e.off = f.buf.size(), e.size = b.size();
    f.buf.insert(f.buf.end(), b.begin(), b.end());
    f.h.ents.push_back(e);
  }
  f.h.end = f.buf.size();
  return f;
}
static int WidthOf(const TexSet& ts, const char* name) {
  Texture t;
  std::string why;
  return ts.Peek(name, t, why) ? int(t.width) : -1;
}

static void DirOrder(const std::string& plant) {
  auto add = [&](TexSet& ts, const Fixture& f, bool character) {
    if (plant == "id_order") {   // the planted twin: the rule before S111
      for (const pac::Ent& e : f.h.ents)
        if (((e.id >= 10 && e.id <= 19) || e.id == 39) && e.size >= 0x30) ts.AddDir(pac::Decode(f.buf.data() + e.off, e.size));
      return;
    }
    load_detail::AddTexDirs(ts, f.buf, f.h, character);
  };
  // A character PACH, entries stored in id order: `dup` sits in 12 (8 wide) and 39 (16 wide): the game's list is 10, 11, 39, 12, so 39's wins.
  const Fixture ch = MakePach({{10, TexDir({{"normal", 4}})},
                               {11, TexDir({{"face", 4}})},
                               {12, TexDir({{"color", 8}, {"dup", 8}})},
                               {13, TexDir({{"mask1", 4}})},
                               {17, TexDir({{"late", 4}})},
                               {39, TexDir({{"dup", 16}, {"extra", 4}})}});
  TexSet c;
  add(c, ch, true);
  Check(WidthOf(c, "normal") == 4 && WidthOf(c, "face") == 4 && WidthOf(c, "color") == 8 && WidthOf(c, "extra") == 4, "character: entries 10, 11, 12 and 39 are read");
  Check(WidthOf(c, "dup") == 16, "character: a name in 39 and 12 is 39's (the game's list 10, 11, 39, 12; first match wins)");
  Check(!c.Has("mask1") && !c.Has("late"), "character: entries 13 and 17 are not in the list (the masks join the Threads editor's load only)");
  // An entrance prop PACH: 10 and 20 (cube_00 in both: 10's wins), and a stray 11 the game's prop reader does not read.
  const Fixture pr = MakePach({{10, TexDir({{"diffuse", 4}, {"cube_00", 4}})},
                               {11, TexDir({{"stray", 4}})},
                               {20, TexDir({{"cube_00", 8}, {"cube_ref", 8}})}});
  TexSet p;
  add(p, pr, false);
  Check(WidthOf(p, "diffuse") == 4 && WidthOf(p, "cube_ref") == 8, "event prop: entries 10 and 20 are read");
  Check(WidthOf(p, "cube_00") == 4, "event prop: a name in 10 and 20 is 10's");
  Check(!p.Has("stray"), "event prop: entry 11 is not read (MakeWeapon reads 10, 20)");
  const std::vector<uint32_t>& ck = tex_dir_order::Keys(tex_dir_order::Owner::kCharacter);
  const std::vector<uint32_t>& pk = tex_dir_order::Keys(tex_dir_order::Owner::kEventProp);
  Check(ck == std::vector<uint32_t>{10, 11, 39, 12} && pk == std::vector<uint32_t>{10, 20}, "the two orders are the game's lists");
  // S111 MIPLEVELS (review): the reader's level count rides with the directory (TexSet::AddDirLevels); Get and Peek both hand it on (the blood list reads its texture with Peek).
  auto levels_of = [](const TexSet& ts, const char* name) { Texture t; std::string why; return ts.Peek(name, t, why) ? int(t.game_levels) : -1; };
  Check(levels_of(c, "dup") == 4 && levels_of(c, "normal") == 4, "character: its directories carry a stock load's 4 levels (level_tag::kStockTexLevels: the fresh request's byte), seen through Peek");
  Model cm;
  const int gi = c.Get(cm, "dup");
  Check(gi >= 0 && cm.textures[size_t(gi)].game_levels == 4, "character: Get hands the directory's count to the model's texture");
  TexSet c1;   // S111 LEVELSBYTE: a load whose request byte was 1 (the Threads preview's)
  load_detail::AddTexDirs(c1, ch.buf, ch.h, true, 1);
  Check(levels_of(c1, "dup") == 1 && levels_of(c1, "normal") == 1, "character: the load's count handed to AddTexDirs reaches every character directory (LEVELSBYTE)");
  TexSet p1;
  load_detail::AddTexDirs(p1, pr.buf, pr.h, false, 1);
  Check(levels_of(p1, "diffuse") == 3 && levels_of(p1, "cube_ref") == 3, "event prop: a character count handed to AddTexDirs is not given to an event prop's directories (MakeWeapon's 3)");
  Check(levels_of(p, "diffuse") == 3 && levels_of(p, "cube_ref") == 3, "event prop: entries 10 and 20 carry MakeWeapon's 3 (prop_levels.h; no entry-100 table here)");
  TexSet b;
  b.AddDirLevels(TexDir({{"blood01", 64}}), kBloodTexLevels);
  Check(levels_of(b, "blood01") == 3, "blood: the key-32010 directory at the stage list's 3 (kBloodTexLevels) reaches the texture Peek reads");
}

// A synthetic uncompressed evtobj.pac: EPAC header, the directory rows below, then PACH k at 0x4000 + k x 0x800 (one entry 10 each).
// Rows: SOBJ 2001 -> PACH 0; BOBJ 2101 -> 1, 3101 -> 2, 5101 -> 3 (title 0), 2102 -> 4, 3102 -> 5 (title 1, no waist model), 2103 -> 6 (no table row);
// SPMT 2101 -> 7. The belts sit at PACHs 1..6, nowhere near the disc's runs.
static Bytes EvtobjPac() {
  const int npach = 8;
  Bytes f(0x4000 + size_t(npach) * 0x800, 0);
  std::memcpy(f.data(), "EPAC", 4);
  std::memcpy(f.data() + 0x800, "SOBJ", 4);
  size_t o = 0x80C;
  auto row = [&](const char* name, uint32_t sector) {
    std::memcpy(f.data() + o, name, 4);
    PutLe32(f, o + 4, sector);
    PutLe32(f, o + 8, 1);
    o += 12;
  };
  row("LIST", 0), row("2001", 0);
  row("BOBJ", 0), row("2101", 1), row("3101", 2), row("5101", 3), row("2102", 4), row("3102", 5), row("2103", 6);
  row("SPMT", 0), row("2101", 7);
  for (int k = 0; k < npach; ++k) {
    const size_t at = 0x4000 + size_t(k) * 0x800;
    std::memcpy(f.data() + at, "PACH", 4);
    PutLe32(f, at + 4, 1);
    PutLe32(f, at + 8, 10);
    PutLe32(f, at + 12, 0);
    PutLe32(f, at + 16, 4);
    PutLe32(f, at + 20, uint32_t(k));
  }
  return f;
}

static void Belts(const std::string& plant) {
  const belt_twin::Table t103 = {{0, 0, 0, 5101, 2101, 3101, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 2102, 3102, 0, 0, 0, 0, 0, 0}};
  const Bytes hdr = EvtobjPac();
  Check(belt_twin::IdAtStart(hdr, 0x4000 + 1 * 0x800, "BOBJ") == 2101 && belt_twin::IdAtStart(hdr, 0x4000 + 6 * 0x800, "BOBJ") == 2103, "IdAtStart: a BOBJ row's id by its PACH start");
  Check(belt_twin::IdAtStart(hdr, 0x4000, "BOBJ") == -1 && belt_twin::IdAtStart(hdr, 0x4000 + 7 * 0x800, "BOBJ") == -1, "IdAtStart: a SOBJ / SPMT row is not a BOBJ id");
  Check(belt_twin::TwinId(t103, 2101, 3) == 3101 && belt_twin::TwinId(t103, 3101, 5) == 5101 && belt_twin::TwinId(t103, 5101, 2) == 2101, "TwinId: the row's column of the side");
  Check(belt_twin::TwinId(t103, 2102, 5) == -1 && belt_twin::TwinId(t103, 2103, 3) == -1 && belt_twin::TwinId(t103, 2103, 2) == 2103 && belt_twin::TwinId(t103, 2101, 4) == -1,
        "TwinId: an empty column / no row -> -1; its own side -> itself; side 4 -> -1");

  // The production entry: assets::BeltTwin over a game folder holding the synthetic pac.
  const fs::path root = fs::temp_directory_path() / "texdirorder_unit_root";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "pac" / "evt");
  {
    std::ofstream out(root / "pac" / "evt" / "evtobj.pac", std::ios::binary);
    out.write(reinterpret_cast<const char*>(hdr.data()), std::streamsize(hdr.size()));
  }
  launcher::GameFiles files;
  files.SetRoot(root);
  auto twin = [&](int pach, int side) {
    if (plant == "belt_ranges") {   // the planted twin: the rule before S111 (a PACH outside the fixed runs has no twin)
      struct Run { int side, lo, hi; };
      const Run runs[] = {{2, 349, 398}, {3, 399, 450}, {5, 451, 502}};
      for (const Run& r : runs)
        if (r.side == side && pach >= r.lo && pach <= r.hi) return pach;
      return -1;
    }
    return BeltTwin(files, t103, pach, side);
  };
  Check(twin(1, 3) == 2 && twin(1, 5) == 3 && twin(2, 2) == 1 && twin(3, 3) == 2, "BeltTwin: title 0's left / right / waist PACHs find each other by table 103");
  Check(twin(1, 2) == 1 && twin(6, 2) == 6, "BeltTwin: a belt on its own side is itself");
  Check(twin(4, 5) == -1 && twin(6, 3) == -1 && twin(0, 3) == -1 && twin(7, 3) == -1, "BeltTwin: no waist model / no row / a SOBJ or SPMT PACH -> -1");
  Check(BeltTwin(files, belt_twin::Table{}, 1, 3) == -1, "BeltTwin: no table -> -1");
  fs::remove_all(root, ec);
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "selftest") == 0) {
    const std::string plant = argc >= 4 && std::strcmp(argv[2], "--plant") == 0 ? argv[3] : "";
    DirOrder(plant);
    Belts(plant);
    std::printf("%s (plant: %s)\n", g_fail ? "FAIL" : "ALL PASS", plant.empty() ? "none" : plant.c_str());
    return g_fail ? 1 : 0;
  }
  std::printf("usage: texdirorder_unit selftest [--plant id_order|belt_ranges]\n");
  return 2;
}
