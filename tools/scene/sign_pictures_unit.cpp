// S103 CSF: the crowd signs' file pictures (assets::crowd_prop_gs::LoadSignPictures, pac\audience.pac PACH 0 entry 0) against the player's own game files.
//   sign_pictures_unit GAME_ROOT      exit 0 = pass. Reads only; prints the counts and the decoded memory.
// Checks (docs\agents\s103\CS.md section 2, scanned 2026-11-19): 525 pictures, every one 64x64 DXT1 with 2048 mip-0 bytes, keys unique and below 65000, the 16 generic keys 60001, 60011 ... 60151 present,
// a second call returns the same set from the once-per-boot cache, and Load(arena) carries the same sign set beside the arena's own pictures (keys 40000-42999 only).
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/crowd_prop_game_space.h"

namespace gs = assets::crowd_prop_gs;
static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, why.empty() ? "" : "  ", why.c_str());
  if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
  if (argc < 2) return std::printf("usage: sign_pictures_unit GAME_ROOT\n"), 2;
  if (!gs::Init(argv[1])) return std::printf("NEEDS-MAIN  no game files at %s\n", argv[1]), 2;
  std::vector<gs::PropPicture> a, b;
  std::string err;
  const bool ok = gs::LoadSignPictures(a, err);
  Check(ok, "loads", err);
  if (!ok) return 1;
  size_t bytes = 0;
  bool shapes = true, small = true;
  std::set<uint32_t> keys;
  for (const gs::PropPicture& p : a) {
    shapes = shapes && p.width == 64 && p.height == 64 && p.format == 18 && p.bytes.size() == 2048;
    small = small && p.key != 0 && p.key < 65000;
    keys.insert(p.key);
    bytes += p.bytes.size();
  }
  Check(a.size() == 525, "count_525", std::to_string(a.size()));
  Check(shapes, "all_64x64_dxt1_2048");
  Check(small && keys.size() == a.size(), "keys_unique_below_65000");
  bool generic = true;
  for (uint32_t i = 0; i < 16; ++i) generic = generic && keys.count(60001 + 10 * i);
  Check(generic, "generic_16_present");
  Check(gs::LoadSignPictures(b, err) && b.size() == a.size() && (a.empty() || b[0].bytes == a[0].bytes), "cached_second_call");
  gs::PropSet set;
  const bool arena_ok = gs::Load(4, set, err);
  Check(arena_ok && set.sign_pictures.size() == a.size(), "arena_set_carries_signs", arena_ok ? "" : err);
  bool hands_only = arena_ok;
  for (const gs::PropPicture& p : set.pictures) hands_only = hands_only && p.key >= 40000 && p.key < 43000;
  Check(hands_only, "arena_pictures_stay_40000_range");
  std::printf("decoded sign pictures: %zu x 2048 B = %zu bytes (%.2f MB)\n", a.size(), bytes, double(bytes) / (1024.0 * 1024.0));
  std::printf("%s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
