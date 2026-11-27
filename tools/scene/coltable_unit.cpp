// S111 COLTABLE: offline proof of WHICH pac's p0/600 colour table attire_remap.h ReadTable reads (src\assets\attire_table_src.h, docs\research\s111\COLTABLE.md).
// 0 game runs; selftest uses fixture directories and pack sets built here (nothing from the game).
//   coltable_unit.exe selftest                     DlcMsc1In on fixture EPK8 / EPAC directories; EndOrder on fixture pack sets (the game's DLC-pass order)
//   coltable_unit.exe real --game DIR --dlc DIR    read-only, the player's own files: the table read with the packs in use, and with none
// Build: pwsh -NoProfile -File tools\scene\build_coltable_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../../src/assets/attire_remap.h"

namespace fs = std::filesystem;
namespace ar = assets::attire_remap;
namespace ts = assets::attire_table_src;

static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}

static void Put32(assets::Bytes& b, size_t o, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(v >> (8 * i));
}

// A 16 KB pac header with the given directory sections.
struct Sec {
  const char* tag;
  bool wide;   // 16-byte rows with 8-character names
  std::vector<std::pair<const char*, uint32_t>> rows;   // {name, sector}
};
static assets::Bytes Header(const char* magic, const std::vector<Sec>& secs) {
  assets::Bytes b(0x4000, 0);
  std::memcpy(b.data(), magic, 4);
  size_t p = 0x800;
  for (const Sec& s : secs) {
    const size_t rs = s.wide ? 16 : 12;
    std::memcpy(b.data() + p, s.tag, 4);
    Put32(b, p + 4, uint32_t(s.rows.size() * rs / 4) | (s.wide ? 0x10000u : 0u));
    p += 12;
    for (const auto& [name, sector] : s.rows) {
      std::memcpy(b.data() + p, name, std::strlen(name));
      Put32(b, p + rs - 8, sector), Put32(b, p + rs - 4, 1);
      p += rs;
    }
  }
  return b;
}

static int SelfTest() {
  size_t base = 0;
  Check(ts::DlcMsc1In(Header("EPK8", {{"DLC5", false, {{"MSC1", 0}}}, {"BATL", false, {{"SAVE", 3}}}}), base) == 5 && base == 0x4000,
        "a pack's misc pac (section DLC5 entry MSC1 at sector 0): /DLC5/MSC1 = the PACH at 0x4000");
  base = 0;
  Check(ts::DlcMsc1In(Header("EPAC", {{"BATL", false, {{"SAVE", 0}}}, {"DLC7", true, {{"MSC1    ", 2}}}}), base) == 7 && base == 0x4000 + 2 * 0x800,
        "a later section with 8-character rows (space padded): /DLC7/MSC1 at sector 2");
  Check(ts::DlcMsc1In(Header("EPK8", {{"DLC3", false, {{"MSC2", 0}}}}), base) == -1, "DLC3 without an MSC1 entry: none");
  Check(ts::DlcMsc1In(Header("EPK8", {{"DLC0", false, {{"MSC1", 0}}}}), base) == -1, "DLC0 (outside the request loops' 1..9): none");
  Check(ts::DlcMsc1In(Header("EPK8", {{"DLC4", true, {{"MSC1X", 0}}}}), base) == -1, "an 8-character name that only starts with MSC1: none");
  Check(ts::DlcMsc1In(Header("XXXX", {{"DLC4", false, {{"MSC1", 0}}}}), base) == -1, "not an EPAC / EPK8 header: none");
  {
    assets::Bytes b = Header("EPK8", {{"DLC4", false, {}}});
    Put32(b, 0x804, 0xFFFF);   // a section claiming 0x3FFFC bytes of rows
    Check(ts::DlcMsc1In(b, base) == -1, "a section whose rows run past the 16 KB header: none, and no read past it");
  }

  using V = std::vector<size_t>;
  Check(ts::EndOrder({}).empty(), "no pack misc pac: the DLC pass reads none (the disc's table stays)");
  Check(ts::EndOrder({{3, 3}, {4, 4}, {5, 5}}) == V{2, 1, 0}, "packs 3 / 4 / 5 (this PC): the game ends on /DLC5/MSC1, then 4, then 3");
  Check(ts::EndOrder({{5, 5}, {3, 3}}) == V{0, 1}, "the order the packs are given in does not matter: the highest n first");
  Check(ts::EndOrder({{1, 1}, {2, 2}}).empty(), "only /DLC1 and /DLC2 (the start loop is n = 3..9): none read");
  Check(ts::EndOrder({{4, 4}, {1, 1}, {2, 2}}) == V{0, 2}, "first n = 4: the follow-up reads /DLC2 .. /DLC9, never /DLC1");
  Check(ts::EndOrder({{5, 5}, {5, 9}}) == V{1}, "two packs with /DLC5/MSC1: the highest pack id's copy (the pac-list head); the other is never read");
  Check(ts::EndOrder({{-1, 6}, {10, 7}, {9, 8}}) == V{2}, "a pac with no MSC1 section, or n outside 1..9: not a source");

  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  coltable_unit selftest\n");
  return g_fail ? 1 : 0;
}

static int Real(const char* game_dir, const char* dlc_dir) {
  launcher::GameFiles files;
  files.SetRoot(fs::path(game_dir));
  std::printf("      %s\n", assets::dlc_files::Configure(files, fs::path(game_dir), true, fs::path(dlc_dir), "").c_str());
  const std::vector<ts::Src> srcs = ts::DlcSources(files);
  for (const ts::Src& s : srcs) std::printf("      source %s /DLC%d/MSC1 (pack %d, PACH at 0x%zx)\n", s.rel.c_str(), s.n, s.pack, s.base);
  ar::Table t;
  std::string err, from;
  Check(ar::ReadTable(files, t, err, &from), "the table reads with the packs in use");
  std::printf("      from %s: %u records%s%s\n", from.c_str(), t.count, err.empty() ? "" : "; ", err.c_str());
  if (!srcs.empty()) Check(from.rfind(srcs[0].rel, 0) == 0, "with packs: the table comes from the highest /DLC<n>/MSC1 (" + srcs[0].rel + "), not the disc");
  files.SetOverlays({});
  ar::Table d;
  std::string derr, dfrom;
  Check(ar::ReadTable(files, d, derr, &dfrom) && dfrom == std::string(ts::kDiscPac) + " /BATL/MSC1", "with no pack in use: the disc's misc01_Start.pac (/BATL/MSC1), as before");
  std::printf("      disc: %u records\n", d.count);
  int missing = 0, differ = 0;
  for (uint32_t k = 0; k < d.count; ++k) {
    const uint8_t* r = d.data.data() + 2 + ar::kRecord * k;
    const uint8_t* q = t.Record(int(ar::Table::Id(r)));
    if (!q) ++missing;
    else if (std::memcmp(r, q, ar::kRecord) != 0) ++differ;
  }
  std::printf("      disc records absent from the packs' table: %d; present but different: %d\n", missing, differ);
  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  coltable_unit real\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "selftest")) return SelfTest();
  if (argc >= 6 && !std::strcmp(argv[1], "real") && !std::strcmp(argv[2], "--game") && !std::strcmp(argv[4], "--dlc")) return Real(argv[3], argv[5]);
  std::printf("usage: coltable_unit selftest | real --game DIR --dlc DIR\n");
  return 2;
}
