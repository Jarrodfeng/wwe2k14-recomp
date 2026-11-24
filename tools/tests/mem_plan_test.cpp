// entry: wwe2k14::mem_plan::Apply
// Unit test for src/mem_plan_core.h (S110 REPLAN) + static checks of the hook. Build: clang++ -std=c++20 -I src
// tools/tests/mem_plan_test.cpp (tools\tests\mem_plan_test.ps1 does it and passes the repo root as argv[1]). 0 game runs.
// The planted twin: a synthetic word map laid out like the game's seven pair tables and the config block (each table's ids in the
// game's order, ids 22..42 at the game's sizes from the core's own table, sizes of our own for every other id, with the one property
// the game's own tables have: the 22..43 re-cut tables sum to the boot table's 22..43). On it the layouts are cut the way the game
// cuts them (heaps downward from the block's top, heap 55 the rest; the re-cuts from heap 14 with heap 54 the rest down to heap 55;
// the 22..43 re-cuts from heap 22; windows upward from the span's base) and compared, game sizes against the plan.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "mem_plan_core.h"
namespace mpl = wwe2k14::mem_plan;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

struct Mem {
  std::map<uint32_t, uint32_t> w;
  bool Rd(uint32_t a, uint32_t* v) const {
    const auto it = w.find(a);
    if (it == w.end()) return false;
    *v = it->second;
    return true;
  }
};
static const uint32_t kCfg = 0x8392479Cu;
static const uint32_t kBootHeaps = 0x83625BF8u, kD70 = 0x83625D70u, kE28 = 0x83625E28u, kEE0 = 0x83625EE0u, kF78 = 0x83625F78u;
static const uint32_t kBootWins = 0x83626020u, kCawWins = 0x83626190u;

// Each table's ids in the game's pair order.
static std::vector<uint32_t> Ids(uint32_t addr) {
  std::vector<uint32_t> v;
  if (addr == kBootHeaps) {
    for (uint32_t k = 0; k <= 44; ++k) v.push_back(k);
    v.push_back(54);
  } else if (addr == kD70 || addr == kE28) {
    for (uint32_t k = 22; k <= 44; ++k) v.push_back(k);
  } else if (addr == kEE0 || addr == kF78) {
    v = {14, 22, 23, 27, 34, 35, 39, 21, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53};
  } else if (addr == kBootWins) {
    for (uint32_t k = 0; k <= 20; ++k) v.push_back(k);
    for (uint32_t k = 22; k <= 44; ++k) v.push_back(k);
    v.push_back(54);
  } else if (addr == kCawWins) {
    v = {14, 22, 23, 27, 34, 35, 39};
    for (uint32_t k = 44; k <= 53; ++k) v.push_back(k);
  }
  return v;
}
static const mpl::Table& T(uint32_t addr) {
  for (const mpl::Table& t : mpl::kTables)
    if (t.addr == addr) return t;
  std::printf("no table 0x%08X\n", addr);
  return mpl::kTables[0];
}
static uint32_t GameKb(uint32_t addr, uint32_t id) {
  return id >= mpl::kIdFirst && id < mpl::kIdFirst + mpl::kIdCount ? T(addr).game_kb[id - mpl::kIdFirst] : 0;
}
// Our own sizes for the ids the plan does not touch. Heap 43 in the 22..43 re-cut tables takes what their absent slots free, as the
// game's does (its 22..43 sums equal the boot table's).
static uint32_t OwnKb(uint32_t addr, uint32_t id) {
  if (id == 44 && (addr == kBootHeaps || addr == kD70 || addr == kE28)) return 0;
  if (id == 43 && (addr == kD70 || addr == kE28)) {
    uint32_t freed = 0;
    for (uint32_t i = 0; i < mpl::kIdCount; ++i) freed += T(kBootHeaps).game_kb[i] - T(addr).game_kb[i];
    return 4000 + freed;
  }
  if (id == 43) return 4000;
  return 100 + id + (addr & 0xFF);
}
static Mem GameLike() {
  Mem m;
  for (const mpl::Table& t : mpl::kTables) {
    const std::vector<uint32_t> ids = Ids(t.addr);
    if (ids.size() != t.pairs) std::printf("twin: table 0x%08X has %zu ids, the core says %u\n", t.addr, ids.size(), t.pairs);
    for (uint32_t k = 0; k < ids.size(); ++k) {
      const uint32_t id = ids[k];
      const bool planned = id >= mpl::kIdFirst && id < mpl::kIdFirst + mpl::kIdCount;
      m.w[t.addr + 8 * k] = id;
      m.w[t.addr + 8 * k + 4] = planned ? GameKb(t.addr, id) : OwnKb(t.addr, id);
    }
  }
  m.w[kCfg + mpl::kBlockWord] = mpl::kGameBlock;
  m.w[kCfg + mpl::kSpanWord] = mpl::kGameSpan;
  return m;
}
static auto RdOf(Mem& m) {
  return [&m](uint32_t a, uint32_t* v) { return m.Rd(a, v); };
}
static auto WrOf(Mem& m) {
  return [&m](uint32_t a, uint32_t v) { m.w[a] = v; };
}
// The KB of id `id` in table `addr` (0 when absent), and the sum of ids lo..hi.
static uint32_t Kb(const Mem& m, uint32_t addr, uint32_t id) {
  const std::vector<uint32_t> ids = Ids(addr);
  for (uint32_t k = 0; k < ids.size(); ++k)
    if (m.w.at(addr + 8 * k) == id) return m.w.at(addr + 8 * k + 4);
  return 0;
}
static int64_t Sum(const Mem& m, uint32_t addr, uint32_t lo, uint32_t hi) {
  int64_t s = 0;
  for (uint32_t id = lo; id <= hi; ++id) s += Kb(m, addr, id);
  return s;
}
// The layouts, in KB.
struct Layout {
  int64_t heap55;            // boot: the block minus heaps 0..54
  int64_t caw_heap54[2];     // Create a Superstar re-cut (EE0, F78): heap 54 = heap 14's top down to heap 55's top, minus 14..53
  int64_t recut_gap[2];      // 22..43 re-cuts (D70, E28): boot 22..43 minus the re-cut's 22..43 (>= 0: heap 43 stays above heap 54)
  int64_t win55;             // boot: the span minus windows 0..54
  int64_t caw_win54;         // Create a Superstar re-cut: window 54 = the span minus boot 0..13 minus re-cut 14..53
  int64_t low_free;          // guest physical between 0x01000000 and the block's bottom
};
static Layout Cut(const Mem& m) {
  Layout l{};
  const int64_t block = m.w.at(kCfg + mpl::kBlockWord) >> 10, span = m.w.at(kCfg + mpl::kSpanWord) >> 10;
  l.heap55 = block - Sum(m, kBootHeaps, 0, 54);
  const int64_t below14 = block - Sum(m, kBootHeaps, 0, 13) - l.heap55;  // heap 14's top down to heap 55's top
  l.caw_heap54[0] = below14 - Sum(m, kEE0, 14, 53);
  l.caw_heap54[1] = below14 - Sum(m, kF78, 14, 53);
  l.recut_gap[0] = Sum(m, kBootHeaps, 22, 43) - Sum(m, kD70, 22, 43);
  l.recut_gap[1] = Sum(m, kBootHeaps, 22, 43) - Sum(m, kE28, 22, 43);
  l.win55 = span - Sum(m, kBootWins, 0, 54);
  l.caw_win54 = span - Sum(m, kBootWins, 0, 13) - Sum(m, kCawWins, 14, 53);
  l.low_free = ((int64_t(mpl::kSpanTop) >> 10) - span - block) - (int64_t(mpl::kGuestFloor) >> 10);
  return l;
}
static std::string Slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  for (size_t r; (r = s.find('\r')) != std::string::npos;) s.erase(r, 1);
  return s;
}
static size_t Count(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : ".";
  // (a) the game's tables: every planned word raised to its budget, the block and the span grown by the boot tables' gain
  {
    Mem m = GameLike();
    const Mem before = m;
    const Layout g = Cut(m);
    const mpl::Plan p = mpl::Apply(RdOf(m), WrOf(m), kCfg);
    CHECK(p.ok);
    CHECK(p.heap_added_kb == 19872);   // 6 x 1472 + 3 x 2144 + 3 x 1536 (data heaps 22..33 only)
    CHECK(p.window_added_kb == 11904); // 6 x 1184 + 3 x 1600
    CHECK(p.block_from == 0x0BD00000u && p.block_to == 0x0D070000u);  // + 19,904 KB (19,872 rounded up to 64 KB)
    CHECK(p.span_from == 0x10400000u && p.span_to == 0x10FA0000u);    // + 11,904 KB
    CHECK(p.free_kb == 12224);         // 0x1FC00000 - 0x10FA0000 - 0x0D070000 = 0x01BF0000: 0xBF0000 above 0x01000000
    CHECK(p.words == 12 + 6 + 12 + 3 + 3 + 9 + 3);
    for (uint32_t id = 22; id <= 42; ++id) {
      // A data heap row the game gives room (22..33, not 0 / absent) is 2560; every instance heap row 34..42 stays the game's.
      for (const uint32_t t : {kBootHeaps, kE28, kD70, kEE0, kF78})
        CHECK(Kb(m, t, id) == (id <= 33 && GameKb(t, id) ? 2560u : GameKb(t, id)));
      const bool caw = id == 22 || id == 23 || id == 27 || id == 34 || id == 35 || id == 39;
      CHECK(Kb(m, kBootWins, id) == (id >= 34 ? 2048u : GameKb(kBootWins, id)));
      CHECK(Kb(m, kCawWins, id) == (caw ? (id >= 34 ? 2048u : 6656u) : 0u));
    }
    CHECK(Kb(m, kD70, 34) == 1294 && Kb(m, kEE0, 34) == 1152 && Kb(m, kBootHeaps, 41) == 492);  // instance heaps: the game's
    size_t changed = 0;
    for (const auto& [a, v] : before.w)
      if (m.w.at(a) != v) ++changed;
    CHECK(changed == p.words + 2);  // the planned words and the two config words; every id and every other size as it was
    CHECK(m.w.size() == before.w.size());
    // (f) the layouts: heap 55 and window 55 keep their sizes (up to the 64 KB rounding); the re-cut rests never shrink; the 22..43
    //     re-cuts stay above heap 54; the low room keeps the reserve
    const Layout l = Cut(m);
    CHECK(l.heap55 - g.heap55 == 19904 - 19872);
    CHECK(l.win55 == g.win55);
    for (int k = 0; k < 2; ++k) {
      CHECK(l.caw_heap54[k] >= g.caw_heap54[k]);
      CHECK(g.recut_gap[k] == 0);  // the twin holds the game's property
      CHECK(l.recut_gap[k] >= 0);
    }
    CHECK(l.caw_win54 >= g.caw_win54);
    CHECK(g.low_free == 44032);     // 0x01000000 - 0x03B00000 at the game's sizes
    CHECK(l.low_free == int64_t(p.free_kb));
    CHECK(l.low_free >= int64_t(mpl::kReserveKb));
    // (b) a second call changes nothing (the config words are no longer the game's)
    const Mem raised = m;
    CHECK(!mpl::Apply(RdOf(m), WrOf(m), kCfg).ok);
    CHECK(m.w == raised.w);
    // (c) Restore puts every table back; the config words stay as asked
    mpl::Restore(WrOf(m), p);
    for (const auto& [a, v] : before.w)
      if (a != kCfg + mpl::kBlockWord && a != kCfg + mpl::kSpanWord) CHECK(m.w.at(a) == v);
    CHECK(m.w.at(kCfg + mpl::kBlockWord) == 0x0D070000u && m.w.at(kCfg + mpl::kSpanWord) == 0x10FA0000u);
  }
  // (d) planted faults, one per table and per config word: nothing written (fall back whole, never half)
  {
    std::vector<Mem> bad;
    for (const mpl::Table& t : mpl::kTables) {
      Mem a = GameLike();  // a planned id's size changed
      const std::vector<uint32_t> ids = Ids(t.addr);
      for (uint32_t k = 0; k < ids.size(); ++k)
        if (ids[k] == 34) a.w[t.addr + 8 * k + 4] += 1;
      bad.push_back(a);
      Mem b = GameLike();  // an id the game leaves out or at 0 given a size, or an id twice
      for (uint32_t k = 0; k < ids.size(); ++k)
        if (ids[k] == 44) b.w[t.addr + 8 * k] = 36;
      bad.push_back(b);
      Mem c = GameLike();  // an unreadable word
      c.w.erase(t.addr + 8 * (t.pairs - 1));
      bad.push_back(c);
    }
    Mem d = GameLike();
    d.w[kCfg + mpl::kBlockWord] = 0x0D8B0000u;  // the S110 PAC176CRASH block (a second plan)
    bad.push_back(d);
    Mem e = GameLike();
    e.w[kCfg + mpl::kSpanWord] = 0x10FA0000u;   // the S110 POOLWIN span
    bad.push_back(e);
    Mem f = GameLike();
    f.w.erase(kCfg + mpl::kSpanWord);
    bad.push_back(f);
    for (Mem& m : bad) {
      const Mem before = m;
      CHECK(!mpl::Apply(RdOf(m), WrOf(m), kCfg).ok);
      CHECK(m.w == before.w);
    }
  }
  CHECK(mpl::RoundUp64KbInKb(19872) == 19904);
  CHECK(mpl::RoundUp64KbInKb(11904) == 11904);
  CHECK(mpl::PlanKb(mpl::Kind::kHeap, 21) == 0 && mpl::PlanKb(mpl::Kind::kHeap, 43) == 0);
  CHECK(mpl::PlanKb(mpl::Kind::kHeap, 33) == 2560 && mpl::PlanKb(mpl::Kind::kHeap, 34) == 0 && mpl::PlanKb(mpl::Kind::kHeap, 42) == 0);
  CHECK(mpl::PlanKb(mpl::Kind::kWindow, 30) == 0 && mpl::PlanKb(mpl::Kind::kWindow, 42) == 2048);
  CHECK(mpl::Describe() == "model data heaps 22-33 2560 KB, instance heaps 34-42 the game's, texture windows 22-33 the game's, "
                           "vertex-buffer windows 34-42 2048 KB");
  // (g) the hook: src/mem_plan.h hooks sub_8225F0F0, applies the plan before the original in the cvar-on path, restores after it;
  //     src/kernel_overrides.h includes it instead of the two per-budget fixes
  {
    const std::string hk = Slurp(root + "/src/mem_plan.h");
    CHECK(Count(hk, "REX_HOOK_RAW(sub_8225F0F0)") == 1);
    CHECK(Count(hk, "__imp__sub_8225F0F0(ctx, base);") == 2);
    const size_t apply = hk.find("plan_ns::Apply(rd, wr, ctx.r3.u32);");
    const size_t call = hk.rfind("__imp__sub_8225F0F0(ctx, base);");
    const size_t restore = hk.find("plan_ns::Restore(wr, plan);");
    CHECK(apply != std::string::npos && call != std::string::npos && restore != std::string::npos);
    CHECK(apply < call && call < restore);
    const std::string ko = Slurp(root + "/src/kernel_overrides.h");
    CHECK(Count(ko, "#include \"mem_plan.h\"") == 1);
    CHECK(Count(ko, "#include \"char_heap_room.h\"") == 0);
    CHECK(Count(ko, "#include \"pool_window_room.h\"") == 0);
  }
  // (h) the REAL image (S110 REPLAN review): argv[2] = out\harness\index\image.bin, the guest image as loaded (base 0x82000000; a
  //     local build artefact written by src\image_dump.h, never committed). The twin above takes its planned rows from the core's own
  //     kTables, so a mistyped row there would pass it (POOLWIN's test table did not match the image). Here every table is read from
  //     the image itself: the ids against the twin's order, each planned id's KB against kTables, then Apply on the image words and
  //     the layouts cut with the game's real size for every id (heap 43, the Create a Superstar rows, heap / window 54 included).
  if (argc > 2) {
    std::ifstream f(argv[2], std::ios::binary);
    const std::string img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto be32 = [&img](uint32_t a, uint32_t* v) {
      if (a < 0x82000000u || uint64_t(a - 0x82000000u) + 4 > img.size()) return false;
      const auto* q = reinterpret_cast<const unsigned char*>(img.data() + (a - 0x82000000u));
      *v = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | uint32_t(q[3]);
      return true;
    };
    Mem m;
    bool read_all = !img.empty();
    for (const mpl::Table& t : mpl::kTables) {
      const std::vector<uint32_t> ids = Ids(t.addr);
      CHECK(ids.size() == t.pairs);
      for (uint32_t k = 0; k < t.pairs && read_all; ++k) {
        uint32_t id = 0, kb = 0;
        read_all = be32(t.addr + 8 * k, &id) && be32(t.addr + 8 * k + 4, &kb);
        m.w[t.addr + 8 * k] = id;
        m.w[t.addr + 8 * k + 4] = kb;
        if (k < ids.size() && id != ids[k]) {
          std::printf("image: table 0x%08X pair %u is id %u, the twin's order says %u\n", t.addr, k, id, ids[k]);
          ++fails;
        }
      }
      for (uint32_t i = 0; i < mpl::kIdCount && read_all; ++i) {
        uint32_t kb = 0;
        for (uint32_t k = 0; k < t.pairs; ++k)
          if (m.w.at(t.addr + 8 * k) == mpl::kIdFirst + i) kb = m.w.at(t.addr + 8 * k + 4);
        if (kb != t.game_kb[i]) {
          std::printf("image: table 0x%08X id %u is %u KB in the image, kTables says %u\n", t.addr, mpl::kIdFirst + i, kb, t.game_kb[i]);
          ++fails;
        }
      }
    }
    CHECK(read_all);
    if (read_all) {
      // The config words are written at run time (sub_827FE8A8 +12C / +108), not image words: the game's constants.
      m.w[kCfg + mpl::kBlockWord] = mpl::kGameBlock;
      m.w[kCfg + mpl::kSpanWord] = mpl::kGameSpan;
      const Layout g = Cut(m);
      const mpl::Plan p = mpl::Apply(RdOf(m), WrOf(m), kCfg);
      CHECK(p.ok);
      CHECK(p.words == 48 && p.heap_added_kb == 19872 && p.window_added_kb == 11904 && p.free_kb == 12224);
      const Layout l = Cut(m);
      CHECK(g.heap55 == 15313);  // 193,536 - 178,223 (heaps 0..54 at the game's sizes)
      CHECK(g.win55 == 4384);    // 266,240 - 261,856 (windows 0..54)
      CHECK(l.heap55 - g.heap55 == 32 && l.win55 == g.win55);
      CHECK(g.caw_heap54[0] == 19241 && g.caw_heap54[1] == 26921);  // boot 14..54 = 106,590; EE0 / F78 14..53 = 87,349 / 79,669
      CHECK(l.caw_heap54[0] - g.caw_heap54[0] == 15456 && l.caw_heap54[1] - g.caw_heap54[1] == 15456);  // 19,872 - 3 x 1472
      CHECK(g.recut_gap[0] == 0 && g.recut_gap[1] == 0);  // the game's 22..43 re-cuts sum to its boot 22..43 (59,664)
      CHECK(l.recut_gap[0] == 11040 && l.recut_gap[1] == 0);  // D70 keeps 28..33 / 40..42 at 0: 11,040 KB stay unused above heap 44
      CHECK(g.caw_win54 == 72160);  // 266,240 - boot 0..13 (91,904) - the Create a Superstar windows 14..53 (102,176)
      CHECK(l.caw_win54 - g.caw_win54 == 8352);
      if (!fails) std::printf("image check: 7 tables read from the image, kTables matches it, layouts cut with the game's real sizes\n");
    }
  } else {
    std::printf("image check SKIPPED: no image path (tools\\tests\\mem_plan_test.ps1 passes out\\harness\\index\\image.bin)\n");
  }
  if (fails) {
    std::printf("MEM PLAN UNIT FAILED: %d\n", fails);
    return 1;
  }
  std::printf("MEM PLAN UNIT PASSED\n");
  return 0;
}
