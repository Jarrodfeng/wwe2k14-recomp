// S110 WALKSINGLE: the offline proof of src\scene_live\walk_single_core.h (docs\research\s110\WALKRACE.md "Sites the build will touch" 6). 0 game runs, no SDK, no game memory beyond the guest image file.
// Build + run (pwsh, after `. tools\env.ps1`): clang++ -O1 -g -std=c++20 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Wno-unused-command-line-argument -I src tools\scene\walk_single_unit.cpp -o out\tools\walk_single_unit.exe && out\tools\walk_single_unit.exe [--plant no_r3_test] [--image PATH]
// Cases (Decide(on, r3, r4, g)): (G, table) -> 0; (G, 0) -> 0; (other object, 1) -> 1; (other object, table) -> unchanged; switch off -> unchanged. The planted twin `no_r3_test` drops the `r3 == g` test: it MUST fail case 4.
// Then the two tables are decoded from the guest image (out\harness\index\image.bin, base 0x82000000; the main checkout's copy when the worktree has none): the range table 0x83625190 (entries of 4 words {key, first category,
// list, weight}, ending at list -1) and the walk order 0x836259B0 (72 category words); asserted: category 57 is key 0, category 36 is key 1, and in key 0's walk order 57 comes after 0-12. No image = that part SKIPS LOUDLY.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "scene_live/walk_single_core.h"

namespace w1 = wwe2k14::walk1;

static int g_fail = 0;
static bool g_plant = false;
static void Expect(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
// the planted twin: no object test (any setter call that passes the table is turned into 0, including other classes' setters)
static uint32_t PlantDecide(bool on, uint32_t, uint32_t r4, uint32_t g) {
  if (on && g != 0 && r4 == w1::kRangeTable) return 0;
  return r4;
}
static uint32_t Dec(bool on, uint32_t r3, uint32_t r4, uint32_t g) { return g_plant ? PlantDecide(on, r3, r4, g) : w1::Decide(on, r3, r4, g); }

static std::vector<uint8_t> LoadImage(const std::string& given) {
  const char* paths[] = {given.c_str(), "out/harness/index/image.bin", "C:/Users/Jarrod/wwe2k14-recomp/out/harness/index/image.bin"};
  for (const char* p : paths) {
    if (!p[0]) continue;
    FILE* f = std::fopen(p, "rb");
    if (!f) continue;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    std::printf("image: %s (%zu bytes)\n", p, d.size());
    return d;
  }
  return {};
}
static bool Word(const std::vector<uint8_t>& d, uint32_t a, uint32_t& out) {
  const uint64_t off = uint64_t(a) - 0x82000000u;
  if (a < 0x82000000u || off + 4 > d.size()) return false;
  out = (uint32_t(d[off]) << 24) | (uint32_t(d[off + 1]) << 16) | (uint32_t(d[off + 2]) << 8) | d[off + 3];
  return true;
}

int main(int argc, char** argv) {
  std::string image;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = !std::strcmp(argv[++i], "no_r3_test");
    else if (!std::strcmp(argv[i], "--image") && i + 1 < argc) image = argv[++i];
  }
  const uint32_t G = 0x83A00000u, OTHER = 0x83A12340u, T = w1::kRangeTable;
  Expect(Dec(true, G, T, G) == 0, "1 (G, match table) -> 0");
  Expect(Dec(true, G, 0, G) == 0, "2 (G, 0) -> 0");
  Expect(Dec(true, OTHER, 1, G) == 1, "3 (other object, 1) -> 1");
  Expect(Dec(true, OTHER, T, G) == T, "4 (other object, match table) -> unchanged");
  Expect(Dec(false, G, T, G) == T, "5 switch off -> unchanged");
  Expect(Dec(true, 0, T, 0) == T, "6 manager word unread (g = 0) -> unchanged");

  const std::vector<uint8_t> img = LoadImage(image);
  if (img.empty()) {
    std::printf("SKIP  the table decode: image.bin not found (tried the worktree and the main checkout). NOT PROVEN.\n");
  } else {
    // range table: entries {key, first category, list, weight}; the entry's range ends at the next entry's first category
    struct E { uint32_t key, first, list; };
    std::vector<E> es;
    for (uint32_t a = T;; a += 16) {
      uint32_t k, f, l, w;
      if (!Word(img, a, k) || !Word(img, a + 4, f) || !Word(img, a + 8, l) || !Word(img, a + 12, w) || es.size() > 64) { es.clear(); break; }
      if (l == 0xFFFFFFFFu) break;
      es.push_back({k, f, l});
    }
    Expect(es.size() == 9, "range table: 9 entries before the -1 end");
    uint32_t endcat = 0;
    Word(img, T + 16 * uint32_t(es.size()) + 4, endcat);   // the terminating entry's first category = one past the last
    Expect(endcat == 72, "range table: ends at category 72");
    auto key_of = [&](uint32_t cat) -> int {
      for (size_t i = 0; i < es.size(); ++i) {
        const uint32_t hi = i + 1 < es.size() ? es[i + 1].first : endcat;
        if (cat >= es[i].first && cat < hi) return int(es[i].key);
      }
      return -1;
    };
    Expect(key_of(57) == 0, "category 57 (the caption) is key 0");
    Expect(key_of(36) == 1, "category 36 (the screen flash) is key 1");
    Expect(key_of(0) == 0 && key_of(13) == 1 && key_of(71) == 0, "W4 spot checks: 0 -> key 0, 13 -> key 1, 71 -> key 0");
    // walk order: 72 category words, a permutation of 0..71
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < 72; ++i) {
      uint32_t c = 0;
      if (!Word(img, 0x836259B0u + 4 * i, c)) break;
      order.push_back(c);
    }
    bool perm = order.size() == 72;
    uint32_t seen[72] = {};
    for (uint32_t c : order) { if (c >= 72 || seen[c]++) perm = false; }
    Expect(perm, "walk order 0x836259B0: a permutation of 0..71");
    auto pos = [&](uint32_t c) { for (size_t i = 0; i < order.size(); ++i) if (order[i] == c) return int(i); return -1; };
    bool after = pos(57) >= 0;
    for (uint32_t c = 0; c <= 12; ++c) after = after && pos(c) >= 0 && pos(c) < pos(57);   // 0-12 are key 0 (above): all come before 57 in the one walk
    Expect(after, "key 0's walk order: category 57 comes after 0-12");
    Expect(pos(36) >= 0 && pos(36) < pos(0), "key 1's category 36 is walked before key 0's 0-12 (the 360 gap)");
  }
  std::printf("walk_single_unit: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
