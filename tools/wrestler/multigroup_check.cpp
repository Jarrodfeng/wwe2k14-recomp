// multigroup_check: lane 179 (Session 101), the wrestlers' restart-marker rule as a unit check. Scans every pac\ch\chNNN.pac of the player's own game files for JBOY parts with SEVERAL index groups
// (offline, no capture needed) and proves, for each one, the rule the Create a Superstar editor's part 4 measured: the draw's index count is sum(groups) + groups - 1 and the index buffer is the
// groups joined by one 0xFFFF marker each (assets::wrestler_geom::DrawIndexCount / BuildIndices); a single-group part is unchanged (BuildIndices == jboy_parts::AllIndices, DrawIndexCount == its
// index count) on EVERY part scanned; and the old flat rule (the PLANT: plain sum, no markers) gives another count / buffer for the multi-group ones, so the wrestlers' class refuses them.
//   multigroup_check --game DIR [--glob SUBSTR]
// Output: MULTI lines (location + counts only, no game data), RESULT line. Exit 0 = every check held, 1 = a check failed, 2 = no input, 3 = usage.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/assets/jboy_parts.h"
#include "../../src/assets/pac.h"
#include "../../src/assets/wrestler_geom.h"
#include "../../src/launcher/disc_image.h"

using namespace assets;
int main(int argc, char** argv) {
  std::string game, sub;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--game") game = argv[i + 1];
    else if (a == "--glob") sub = argv[i + 1];
    else { std::fprintf(stderr, "multigroup_check: unknown argument %s\n", a.c_str()); return 3; }
  }
  if (game.empty()) { std::fprintf(stderr, "usage: multigroup_check --game DIR [--glob SUBSTR]\n"); return 3; }
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(game));
  std::vector<std::string> rels;
  for (const std::string& n : files.List("pac\\ch"))
    if (n.size() > 4 && (n.substr(n.size() - 4) == ".pac" || n.substr(n.size() - 4) == ".PAC")) rels.push_back("pac\\ch\\" + n);
  uint64_t pacs = 0, parts = 0, single = 0, multi = 0, bad = 0, plant_differs = 0;
  for (const std::string& rel : rels) {
    if (!sub.empty() && rel.find(sub) == std::string::npos) continue;
    pac::Pac p;
    std::string e;
    if (!pac::Open(files, rel, p, e)) { std::printf("open failed %s: %s\n", rel.c_str(), e.c_str()); continue; }
    ++pacs;
    int pi = 0;
    for (const pac::Pach& h : p.pachs) {
      for (const pac::Ent& en : h.ents) {
        if (en.size < 64 || en.size > (64u << 20)) continue;
        Bytes blob = pac::Decode(p.bytes.data() + en.off, en.size);
        if (blob.size() < 8 || std::memcmp(blob.data(), "JBOY", 4) != 0) continue;
        jboy_parts::Parts jb;
        std::string e2;
        if (!jboy_parts::Parse(blob, jb, e2)) continue;
        for (const jboy_parts::Part& pt : jb.parts) {
          if (!pt.ok) continue;
          ++parts;
          const std::vector<uint16_t> flat = jboy_parts::AllIndices(pt), joined = wrestler_geom::BuildIndices(pt), planted = wrestler_geom::BuildIndices(pt, true);
          const uint32_t dc = wrestler_geom::DrawIndexCount(pt), dc_flat = wrestler_geom::DrawIndexCount(pt, true);
          bool ok = joined.size() == dc && planted == flat && dc_flat == pt.index_count && flat.size() == pt.index_count;
          if (pt.index_groups <= 1) {   // a single-group part: unchanged, byte for byte
            ++single;
            ok = ok && joined == flat && dc == pt.index_count;
          } else {
            ++multi;
            size_t markers = 0;
            for (uint16_t v : joined) markers += v == 0xFFFF;
            ok = ok && dc == pt.index_count + pt.index_groups - 1 && markers == pt.index_groups - 1 && joined.size() == flat.size() + markers;
            // the markers sit exactly where the groups end
            size_t at = 0;
            for (size_t g = 0; g + 1 < pt.groups.size() && ok; ++g) {
              at += pt.groups[g].count;
              ok = at + g < joined.size() && joined[at + g] == 0xFFFF;
            }
            if (dc != dc_flat || joined != flat) ++plant_differs;
            std::printf("MULTI %s pach %d entry %u part %u vertices %u groups %u sum %u draw_count %u ok=%d\n", rel.c_str(), pi, en.id, pt.mesh_index, pt.vertices, pt.index_groups, pt.index_count, dc, ok ? 1 : 0);
          }
          if (!ok) ++bad;
        }
      }
      ++pi;
    }
  }
  if (!pacs) { std::fprintf(stderr, "multigroup_check: FAIL no pac read under %s\n", game.c_str()); return 2; }
  std::printf("RESULT pacs=%llu parts=%llu single_group=%llu multi_group=%llu bad=%llu plant_differs=%llu\n", (unsigned long long)pacs, (unsigned long long)parts, (unsigned long long)single, (unsigned long long)multi,
              (unsigned long long)bad, (unsigned long long)plant_differs);
  return bad ? 1 : 0;
}
