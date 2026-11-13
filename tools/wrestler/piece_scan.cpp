// piece_scan: lane 134 (Session 101). Finds the JBOY entries of the player's own pacs (any pac, any entry id) that hold a mesh with given vertex / index counts, and prints
// where they are (pac, PACH number, entry id, part, effect, node table). Our code; prints locations and counts only, no game data.
//   piece_scan --game DIR --counts V:I[,V:I...] [--glob SUBSTR]   (every part of the entry must be in the list for "ALL"; "ANY" lists entries with any match)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "../../src/assets/jboy_parts.h"
#include "../../src/assets/pac.h"
#include "../../src/launcher/disc_image.h"

using namespace assets;
int main(int argc, char** argv) {
  std::string game, counts, sub;
  bool stats = false, list = false;
  std::map<std::string, int> tally;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--stats") { stats = true; continue; }
    if (std::string(argv[i]) == "--list") { list = true; continue; }
    if (i + 1 >= argc) break;
    std::string a = argv[i];
    if (a == "--game") game = argv[i + 1];
    else if (a == "--counts") counts = argv[i + 1];
    else if (a == "--glob") sub = argv[i + 1];
    ++i;
  }
  if (game.empty() || (counts.empty() && !stats && !list)) { std::fprintf(stderr, "usage: piece_scan --game DIR --counts V:I[,V:I] [--glob SUBSTR]\n"); return 3; }
  std::vector<std::pair<uint32_t, uint32_t>> want;
  for (size_t p = 0; p < counts.size();) {
    unsigned v, ix; int n = 0;
    if (std::sscanf(counts.c_str() + p, "%u:%u%n", &v, &ix, &n) < 2) break;
    want.push_back({v, ix}); p += size_t(n); if (p < counts.size() && counts[p] == ',') ++p;
  }
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(game));
  std::vector<std::string> rels;
  for (const char* dir : {"pac", "pac\\ch", "pac\\evt", "pac\\bg"})
    for (const std::string& n : files.List(dir))
      if (n.size() > 4 && (n.substr(n.size() - 4) == ".pac" || n.substr(n.size() - 4) == ".PAC")) rels.push_back(std::string(dir) + "\\" + n);
  int nscan = 0;
  for (const std::string& rel : rels) {
    if (!sub.empty() && rel.find(sub) == std::string::npos) continue;
    pac::Pac p; std::string e;
    if (!pac::Open(files, rel, p, e)) { std::printf("open failed %s: %s\n", rel.c_str(), e.c_str()); continue; }
    ++nscan; std::printf("scan %s pachs %zu\n", rel.c_str(), p.pachs.size());
    int pi = 0;
    for (const pac::Pach& h : p.pachs) {
      for (const pac::Ent& en : h.ents) {
        if (en.size < 64 || en.size > (64u << 20)) continue;
        Bytes blob = pac::Decode(p.bytes.data() + en.off, en.size);
        if (blob.size() < 8 || std::memcmp(blob.data(), "JBOY", 4) != 0) continue;
        jboy_parts::Parts jb; std::string e2;
        if (!jboy_parts::Parse(blob, jb, e2)) continue;
        if (stats) { for (const auto& pt : jb.parts) { char k[200]; std::snprintf(k, sizeof k, "effect %s strides %u/%u/%u/%u", pt.effect.c_str(), pt.stride[0], pt.stride[1], pt.stride[2], pt.stride[3]); ++tally[k]; } continue; }
        if (list) {
          const uint8_t* bb = blob.data() + 8; const uint32_t mt = Be32(bb + 0x14);
          for (const auto& pt : jb.parts) {
            std::printf("LIST %s pach %d entry %u part %u v%u i%u %s strides %u/%u/%u/%u rec:", rel.c_str(), pi, en.id, pt.mesh_index, pt.vertices, pt.index_count, pt.effect.c_str(), pt.stride[0], pt.stride[1], pt.stride[2], pt.stride[3]);
            for (int w = 0; w < 46; ++w) { if (w == 3 || (w >= 6 && w < 7)) {} std::printf(" %08X", Be32(bb + mt + pt.mesh_index * 0xB8 + 4 * w)); }
            std::printf("\n");
          }
          continue;
        }
        std::string hit;
        size_t matched = 0;
        for (const auto& pt : jb.parts) {
          for (auto& w : want) if (pt.vertices == w.first && pt.index_count == w.second) { char b[200]; std::snprintf(b, sizeof b, " [part %u v%u i%u effect %s palette %u:", pt.mesh_index, pt.vertices, pt.index_count, pt.effect.c_str(), pt.palette_count); hit += b; for (uint32_t k = 0; k < pt.palette_count && k < 20; ++k) hit += " " + std::to_string(pt.palette[k]); hit += "]"; ++matched; }
        }
        if (!matched) continue;
        std::printf("HIT %s pach %d entry %u parts %zu nodes %u%s | names:", rel.c_str(), pi, en.id, jb.parts.size(), jb.node_count, hit.c_str());
        for (size_t k = 0; k < jb.node_names.size() && k < 12; ++k) std::printf(" %s", jb.node_names[k].c_str());
        std::printf("\n");
      }
      ++pi;
    }
    std::fflush(stdout);
  }
  for (auto& kv : tally) std::printf("STAT %s : %d\n", kv.first.c_str(), kv.second);
  std::printf("scanned %d pacs\n", nscan);
  return 0;
}
