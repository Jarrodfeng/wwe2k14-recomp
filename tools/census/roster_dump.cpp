// roster_dump: lane 167 (Session 101). Reads the player's own pac\ch\chNNN.pac files exactly the way the wrestler roster does (src\assets\wrestler_roster.cpp Build:
// every PACH that has entry 0 is one attire, entries 0 / 26 / 27 / 10000-39999) and prints, per entry and per part, the properties the wrestler join branches on.
// Output is ids, counts, strides and class names (TSV on stdout). It is printed, never committed (game-derived); tools\census\roster_classes.py reads it.
//   roster_dump --game DIR [--ids 343,324]
//   E <char> <attire> <entry> <nodes> <nodehash> <parts>
//   P <char> <attire> <entry> <part> <vertices> <indices> <palette> <groups> <s0> <s1> <s2> <s3> <tiles> <ok> <effect> <palette_max_node> <palette_raw>
// Compile only; no game is run. Build: tools\census\build_roster_dump.ps1
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "../../src/assets/jboy_parts.h"
#include "../../src/assets/pac.h"
#include "../../src/launcher/disc_image.h"

using namespace assets;
static bool Wanted(uint32_t id) { return id == 0 || id == 26 || id == 27 || (id >= 10000 && id < 40000); }
int main(int argc, char** argv) {
  std::string game, ids;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--game") game = argv[i + 1];
    else if (a == "--ids") ids = argv[i + 1];
  }
  if (game.empty()) { std::fprintf(stderr, "usage: roster_dump --game DIR [--ids A,B]\n"); return 3; }
  std::set<int> want;
  for (size_t p = 0; p < ids.size();) { want.insert(std::atoi(ids.c_str() + p)); p = ids.find(',', p); if (p == std::string::npos) break; ++p; }
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(game));
  std::vector<int> chars;
  for (const std::string& n : files.List("pac\\ch")) {
    int id = 0;
    if (n.size() == 9 && (n.compare(0, 2, "ch") == 0 || n.compare(0, 2, "CH") == 0) && std::sscanf(n.c_str() + 2, "%3d", &id) == 1 && id >= 100 && (n.substr(5) == ".pac" || n.substr(5) == ".PAC")) chars.push_back(id);
  }
  std::sort(chars.begin(), chars.end());
  int opened = 0;
  for (int id : chars) {
    if (!want.empty() && !want.count(id)) continue;
    char rel[64];
    std::snprintf(rel, sizeof rel, "pac\\ch\\ch%03d.pac", id);
    pac::Pac p; std::string e;
    if (!pac::Open(files, rel, p, e)) { std::printf("X\t%d\topen failed\n", id); continue; }
    ++opened;
    int attire = 0;
    for (const pac::Pach& h : p.pachs) {
      if (!pac::Find(h, 0)) continue;
      for (const pac::Ent& en : h.ents) {
        if (!Wanted(en.id)) continue;
        Bytes blob = pac::Decode(p.bytes.data() + en.off, en.size);
        if (blob.size() < 8 || std::memcmp(blob.data(), "JBOY", 4) != 0) continue;
        jboy_parts::Parts jb; std::string e2;
        if (!jboy_parts::Parse(blob, jb, e2)) continue;
        uint64_t hh = 1469598103934665603ull;
        for (const std::string& s : jb.node_names) for (unsigned char c : s) hh = (hh ^ c) * 1099511628211ull;
        std::printf("E\t%d\t%d\t%u\t%u\t%016llx\t%zu\n", id, attire, en.id, jb.node_count, (unsigned long long)hh, jb.parts.size());
        for (const auto& pt : jb.parts) {
          const uint8_t* bb = blob.data() + 8; const uint32_t mt = Be32(bb + 0x14);
          const uint32_t raw_pal = Be32(bb + mt + pt.mesh_index * 0xB8 + 0x08);
          uint32_t mx = 0;
          for (uint32_t k = 0; k < pt.palette_count && k < 20; ++k) mx = std::max(mx, pt.palette[k]);
          std::printf("P\t%d\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%d\t%s\t%u\t%u\n", id, attire, en.id, pt.mesh_index, pt.vertices, pt.index_count, pt.palette_count, pt.index_groups,
                      pt.stride[0], pt.stride[1], pt.stride[2], pt.stride[3], pt.tiles ? 1 : 0, pt.ok ? 1 : 0, pt.effect.c_str(), mx, raw_pal);
        }
      }
      ++attire;
    }
  }
  std::printf("D\tpacs\t%d\n", opened);
  return 0;
}
