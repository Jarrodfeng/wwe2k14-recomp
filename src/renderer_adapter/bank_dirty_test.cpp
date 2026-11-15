// S102 T12: GPU-free unit test of bank_dirty.h (the reader's constant-bank change detection, switch wwe2k14_rdr_bank_dirty) and a microbenchmark.
//   clang++ -std=c++20 -O2 -I src\renderer_adapter src\renderer_adapter\bank_dirty_test.cpp -o bank_dirty_test.exe
//   bank_dirty_test [plant 0-7] [bench]     exit 0 = all pass (clean run) / exit 1 = a plant was caught
//
// What it is: a model of reader.h's PushBanks glue (refresh of the marked blocks against the device bytes, the literal cache with its serial, the emit with
// the plan) around the REAL pieces of bank_dirty.h (ChangedBlocks, PlanEmit, EmitPatches). Two models read one synthetic device and one stream of writes:
// OFF (the old plan: touch = dirty | literal blocks, every block compared) and ON. After every draw both models' banks are resolved (def chain + patches)
// and compared with the ORACLE (the device floats + that draw's literals, built straight from the device), and at every frame's end the def / patch / full-bank
// tables of ON and OFF are compared byte for byte. NOT covered here (needs the game, see the hand-back): reader.h's glue itself (it is compiled by the game exe;
// this model is a copy of its control flow), the SDK's writers (the marks' completeness is the S100 verify run, census bank_change_unmarked = 0), real draw mixes.
//
// Write paths modelled (each marks the 64-byte blocks its dwords touch, as the library's setters do): a vec4 range (SetVertex/PixelShaderConstantF), one dword
// (the single-float writers), a block-aligned group, a range that crosses a block boundary, a rewrite of the whole bank, a write of EQUAL values (marks, no change),
// a partial write (some dwords equal, some not), and a device pointer change (the new device holds the same bytes, or not). Frame boundaries reset the tables and
// the reader's "full compare at a frame part's first draw".
// Plants (each must be caught, exit 1): 1 a write path that never marks (single dwords); 2 a dirty range one register (vec4) short; 3 literals treated as
// unchanged after the table changed; 4 dirty taken as "differs" after a device change; 5 dirty literal blocks taken as "differs" (no compare);
// 6 blocks only the last emit's literals covered not revisited; 7 the inline compare one dword short (60 of 64 bytes); 8 the device-change reset follows the previous draw's switch value (found by the flip scenarios).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bank_dirty.h"

using namespace rdr_adapter;
namespace bd = rdr_adapter::bank_dirty;
namespace cap = rdr_adapter;

static int g_fail = 0;
static int g_plant = 0;
#define CHECK(c, ...) do { if (!(c)) { if (g_fail < 12) { std::printf("FAIL %s:%d %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } ++g_fail; } } while (0)

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); }
  uint32_t Below(uint32_t n) { return Next() % n; }
  bool Chance(uint32_t pct) { return Below(100) < pct; }
};

// ---- the synthetic device ----------------------------------------------------------------------------------------------------------------------
struct Device {  // the device's float shadows: 4 KB per stage, the big-endian words as the game stores them
  uint8_t raw[2][4096];
};
static uint32_t Bswap(uint32_t v) { return __builtin_bswap32(v); }
static uint32_t HostWord(const Device& d, int stage, uint32_t dword) { uint32_t v; std::memcpy(&v, d.raw[stage] + 4 * dword, 4); return Bswap(v); }

// Literal tables: per stage a list of (dword within the bank, value).
struct LitTable {
  std::vector<std::pair<uint32_t, uint32_t>> e[2];
  uint64_t mask[2] = {0, 0};
  std::vector<std::pair<uint32_t, uint32_t>> by_block[2][64];
};
static LitTable MakeTable(Rng& r, uint32_t density) {
  LitTable t;
  for (int s = 0; s < 2; ++s) {
    const uint32_t n = r.Below(density + 1);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t dword = r.Below(1024);
      t.e[s].push_back({dword, r.Next()});
    }
    for (auto& x : t.e[s]) {
      t.mask[s] |= 1ull << (x.first >> 4);
      t.by_block[s][x.first >> 4].push_back(x);  // table order kept, a later entry wins
    }
  }
  return t;
}

// ---- the model of PushBanks ----------------------------------------------------------------------------------------------------------------------
struct Frame {  // the frame tables (reader g_frame)
  std::vector<cap::BankDef> defs;
  std::vector<cap::BankPatch> patches;
  std::vector<cap::ConstantBank> fulls;
  std::vector<std::pair<uint32_t, uint32_t>> draws;  // (vs def, ps def) per draw
};

struct Model {
  bool on = false;
  bool marks_on = true;  // wwe2k14_rdr_bank_marks
  const Device* dev = nullptr;
  bool valid = false;
  bd::State st;  // the reader's own bookkeeping (bank_dirty.h State): the very calls reader.h makes
  bool prev_on = false;
  uint8_t raw[2][4096];
  cap::ConstantBank base[2], last[2];
  uint64_t dirty[2] = {~0ull, ~0ull}, lit_last[2] = {0, 0};
  uint32_t last_def[2] = {cap::kBankNoBase, cap::kBankNoBase};
  uint64_t version = 0, built_version = ~0ull, built_table = ~0ull, built_frame = ~0ull, defs_frame = ~0ull, full_generation = ~0ull;
  uint32_t vs_index = 0, ps_index = 0;
  // the literal cache
  uint64_t lc_generation = ~0ull, lc_table = ~0ull;
  uint64_t marked[2] = {0, 0};
  Frame* frame = nullptr;
  bd::Counters ctr;
  uint64_t emits_full = 0;
};

static void ApplyBlocks(Model& m, int s, const uint8_t* dev, uint64_t mask, bool& changed) {
  for (uint64_t x = mask; x; x &= x - 1) {
    const uint32_t b = uint32_t(std::countr_zero(x));
    std::memcpy(m.raw[s] + 64 * b, dev + 64 * b, 64);
    for (uint32_t k = 0; k < 16; ++k) {
      uint32_t v;
      std::memcpy(&v, dev + 64 * b + 4 * k, 4);
      m.base[s].dwords[16 * b + k] = Bswap(v);
    }
  }
  if (mask) {
    m.dirty[s] |= mask;
    changed = true;
  }
}

// Plant 7: the inline compare one dword short.
static uint64_t ChangedPlant7(const uint8_t* dev, const uint8_t* raw, uint64_t cand) {
  uint64_t mask = 0;
  for (uint64_t x = cand; x; x &= x - 1) {
    const uint32_t b = uint32_t(std::countr_zero(x));
    if (std::memcmp(dev + 64 * b, raw + 64 * b, 60) != 0) mask |= 1ull << b;
  }
  return mask;
}

// One draw: the control flow of reader.h PushBanks with the same pieces. `table_id` = the shader pair (its literal table), `generation` = the frame part.
static void PushBanks(Model& m, const Device* dev, const std::vector<LitTable>& tables, uint64_t table_id, uint64_t gen) {
  bool changed = false;
  bool full = !m.marks_on || m.full_generation != gen;
  if (!m.valid || m.dev != dev) {
    std::memset(m.raw, 0xA5, sizeof(m.raw));
    m.dev = dev;
    m.valid = true;
    if (g_plant == 4) {}                         // plant 4: forgotten
    else if (g_plant == 8) { if (m.prev_on) m.st.OnDeviceChange(); }  // plant 8: the reset follows the PREVIOUS draw's switch value (found by the flip scenarios)
    else m.st.OnDeviceChange();
    full = true;
    m.full_generation = ~0ull;
  }
  const uint64_t marked[2] = {m.marked[0], m.marked[1]};
  m.marked[0] = m.marked[1] = 0;
  for (int s = 0; s < 2; ++s) {
    uint64_t mask;
    if (full) {
      mask = 0;
      for (uint32_t b = 0; b < 64; ++b) {
        if (std::memcmp(dev->raw[s] + 64 * b, m.raw[s] + 64 * b, 64) != 0) mask |= 1ull << b;
      }
    } else {
      mask = m.on ? (g_plant == 7 ? ChangedPlant7(dev->raw[s], m.raw[s], marked[s]) : bd::ChangedBlocks(dev->raw[s], m.raw[s], marked[s], true, &m.ctr))
                  : bd::ChangedBlocks(dev->raw[s], m.raw[s], marked[s], false);
    }
    ApplyBlocks(m, s, dev->raw[s], mask, changed);
  }
  if (full) m.full_generation = gen;
  if (changed) ++m.version;
  Frame& t = *m.frame;
  if (m.built_version == m.version && m.built_table == table_id && m.built_frame == gen) return;  // banks reused
  const LitTable& lt = tables[table_id];
  if (m.lc_generation == gen && m.lc_table == table_id) {
    // reused
  } else {
    m.lc_generation = gen;
    m.lc_table = table_id;
    m.st.OnLitRebuild();
  }
  const bool lits_same = (g_plant == 3 && m.st.lit_serial_last != ~0ull) ? true : m.st.LitsSame();
  if (m.defs_frame != gen) {
    m.defs_frame = gen;
    m.last_def[0] = m.last_def[1] = cap::kBankNoBase;
  }
  if (m.on) {
    m.ctr.emits += 2;
    if (!m.st.dirty_exact) {
      m.ctr.emits_old_plan += 2;
    }
  }
  auto block_of = [&](int s, uint32_t b, uint32_t out[16]) {
    std::memcpy(out, m.base[s].dwords + 16 * b, 64);
    for (auto& x : lt.by_block[s][b]) out[x.first & 15] = x.second;
  };
  uint32_t idx[2];
  for (int s = 0; s < 2; ++s) {
    if (m.last_def[s] == cap::kBankNoBase) {
      cap::BankDef d;
      d.full = uint32_t(t.fulls.size());
      cap::ConstantBank& fb = t.fulls.emplace_back(m.base[s]);
      for (auto& x : lt.e[s]) fb.dwords[x.first] = x.second;
      m.last[s] = fb;
      m.last_def[s] = uint32_t(t.defs.size());
      t.defs.push_back(d);
      ++m.emits_full;
    } else {
      bd::Plan plan = bd::PlanEmit(m.on, m.dirty[s], lt.mask[s], m.lit_last[s], lits_same, m.st.dirty_exact);
      if (m.on && !plan.old_plan) {
        if (g_plant == 5) plan.known_diff = plan.visit & m.dirty[s];
        if (g_plant == 6) plan.visit &= ~(m.lit_last[s] & ~lt.mask[s] & ~m.dirty[s]);
        plan.known_diff &= plan.visit;
      }
      const uint64_t touch = m.dirty[s] | lt.mask[s] | m.lit_last[s];
      m.last_def[s] = bd::EmitPatches(t.defs, t.patches, plan, uint64_t(std::popcount(touch)), [&](uint32_t b, uint32_t out[16]) { block_of(s, b, out); }, m.last[s],
                                      m.last_def[s], m.on ? &m.ctr : nullptr);
    }
    idx[s] = m.last_def[s];
  }
  m.vs_index = idx[0];
  m.ps_index = idx[1];
  m.st.AfterEmit();
  m.prev_on = m.on;
  m.dirty[0] = m.dirty[1] = 0;
  m.lit_last[0] = lt.mask[0];
  m.lit_last[1] = lt.mask[1];
  m.built_version = m.version;
  m.built_table = table_id;
  m.built_frame = gen;
}

// ---- resolving a def chain ---------------------------------------------------------------------------------------------------------------------------
static void Resolve(const Frame& f, uint32_t def, cap::ConstantBank& out) {
  const cap::BankDef& d = f.defs[def];
  if (d.base == cap::kBankNoBase) {
    out = f.fulls[d.full];
    return;
  }
  Resolve(f, d.base, out);
  for (uint32_t i = 0; i < d.patch_count; ++i) {
    const cap::BankPatch& p = f.patches[d.patch_first + i];
    std::memcpy(out.dwords + 16 * p.block, p.dwords, 64);
  }
}

// ---- the write stream ---------------------------------------------------------------------------------------------------------------------------------
// Marks the 64-byte blocks the write [dword, dword + count) touches. Plant 2: the range ends one vec4 register short.
static uint64_t BlocksOf(uint32_t dword, uint32_t count) {
  uint32_t last_dword = dword + count - 1;
  if (g_plant == 2 && count >= 8 && last_dword >= 4) last_dword -= 4;  // one register short: the last register of a range may begin a block
  uint64_t m = 0;
  for (uint32_t b = dword >> 4; b <= (last_dword >> 4); ++b) m |= 1ull << b;
  return m;
}

struct Writer {
  Rng& r;
  Device* dev;
  Model* models[2];
  void Mark(int s, uint32_t dword, uint32_t count, bool marks) {
    if (!marks) return;
    const uint64_t m = BlocksOf(dword, count);
    for (Model* x : models) x->marked[s] |= m;
  }
  void Store(int s, uint32_t dword, uint32_t v) { const uint32_t be = Bswap(v); std::memcpy(dev->raw[s] + 4 * dword, &be, 4); }
  void Write(int kind) {
    const int s = int(r.Below(2));
    uint32_t start = 0, count = 0;
    switch (kind) {
      case 0: start = 4 * r.Below(256); count = 4 * (1 + r.Below(8)); break;                       // a vec4 range
      case 1: start = r.Below(1024); count = 1; break;                                              // one dword
      case 2: start = 16 * r.Below(64); count = 16 * (1 + r.Below(3)); break;                      // block-aligned groups
      case 3: start = 16 * r.Below(63) + 8 + 4 * r.Below(2); count = 4 * (2 + r.Below(5)); break;  // crosses a block boundary
      case 4: start = 0; count = 1024; break;                                                       // the whole bank
      default: start = 4 * r.Below(256); count = 4 * (1 + r.Below(2)); break;                      // (equal-value rewrite below)
    }
    if (start + count > 1024) count = 1024 - start;
    const uint32_t mode = r.Below(3);  // 0 new values, 1 equal values (marks, no change), 2 partial: every other dword equal
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t now = HostWord(*dev, s, start + i);
      uint32_t v = r.Next();
      if (mode == 1 || (mode == 2 && (i & 1))) v = now;
      Store(s, start + i, v);
    }
    Mark(s, start, count, !(g_plant == 1 && kind == 1));  // plant 1: the single-dword writers never mark
  }
};

struct Config {
  uint32_t seed = 1;
  uint32_t frames = 60;
  uint32_t draws_per_frame = 200;
  uint32_t lit_density = 12;
  uint32_t same_table_pct = 90;
  uint32_t write_pct = 60;
  uint32_t dev_change_pct = 2;
  bool marks_on = true;
  uint32_t tables = 8;
  int flip = 0;  // 0 the ON model stays on; 1 its switch flips per frame; 2 per draw (the OFF model stays off)
};

struct Totals {
  uint64_t draws = 0, banks_checked = 0;
};

static Totals RunScenario(const Config& cfg) {
  Rng r(cfg.seed);
  static Device dev_a, dev_b;
  Device* devs[2] = {&dev_a, &dev_b};
  int cur = 0;
  for (auto* d : devs)
    for (int s = 0; s < 2; ++s)
      for (uint32_t i = 0; i < 1024; ++i) {
        const uint32_t be = Bswap(r.Next());
        std::memcpy(d->raw[s] + 4 * i, &be, 4);
      }
  std::vector<LitTable> tables;
  for (uint32_t i = 0; i < cfg.tables; ++i) tables.push_back(MakeTable(r, cfg.lit_density));
  Model off, on;
  off.on = false;
  on.on = true;
  off.marks_on = on.marks_on = cfg.marks_on;
  Writer w{r, devs[cur], {&off, &on}};
  Totals tot;
  uint64_t table = 0, gen = 0;
  Frame f_off, f_on;
  off.frame = &f_off;
  on.frame = &f_on;
  for (uint32_t fr = 0; fr < cfg.frames; ++fr) {
    ++gen;
    if (cfg.flip == 1) on.on = r.Below(3) != 0;
    f_off = Frame();
    f_on = Frame();
    for (uint32_t d = 0; d < cfg.draws_per_frame; ++d) {
      // writes since the last draw
      while (r.Chance(cfg.write_pct)) w.Write(int(r.Below(6)));
      if (r.Chance(cfg.dev_change_pct)) {
        cur ^= 1;
        w.dev = devs[cur];
        if (r.Chance(50)) std::memcpy(devs[cur], devs[cur ^ 1], sizeof(Device));  // the new device holds the same bytes
        else if (r.Chance(50)) { for (int i = 0; i < 40; ++i) w.Write(int(r.Below(6))); }
      }
      if (!r.Chance(cfg.same_table_pct)) table = r.Below(cfg.tables);
      else if (r.Chance(3)) table = (table + 1) % cfg.tables;
      PushBanks(off, w.dev, tables, table, gen);
      if (cfg.flip == 2) on.on = r.Below(4) != 0;
      PushBanks(on, w.dev, tables, table, gen);
      f_off.draws.push_back({off.vs_index, off.ps_index});
      f_on.draws.push_back({on.vs_index, on.ps_index});
      ++tot.draws;
      // the oracle: the device floats + the draw's literals
      for (int s = 0; s < 2; ++s) {
        cap::ConstantBank want;
        for (uint32_t i = 0; i < 1024; ++i) want.dwords[i] = HostWord(*w.dev, s, i);
        for (auto& x : tables[table].e[s]) want.dwords[x.first] = x.second;
        cap::ConstantBank got_off, got_on;
        Resolve(f_off, s ? off.ps_index : off.vs_index, got_off);
        Resolve(f_on, s ? on.ps_index : on.vs_index, got_on);
        CHECK(std::memcmp(&got_off, &want, sizeof(want)) == 0, "OFF bank differs from the oracle: frame %u draw %u stage %d", fr, d, s);
        CHECK(std::memcmp(&got_on, &want, sizeof(want)) == 0, "ON bank differs from the oracle: frame %u draw %u stage %d", fr, d, s);
        tot.banks_checked += 2;
      }
    }
    // frame boundary: the tables of both models are byte-identical
    CHECK(f_off.defs.size() == f_on.defs.size() && f_off.patches.size() == f_on.patches.size() && f_off.fulls.size() == f_on.fulls.size(),
          "table sizes differ at frame %u: defs %zu/%zu patches %zu/%zu", fr, f_off.defs.size(), f_on.defs.size(), f_off.patches.size(), f_on.patches.size());
    if (f_off.defs.size() == f_on.defs.size() && f_off.patches.size() == f_on.patches.size() && f_off.fulls.size() == f_on.fulls.size()) {
      CHECK(std::memcmp(f_off.defs.data(), f_on.defs.data(), f_off.defs.size() * sizeof(cap::BankDef)) == 0, "defs differ at frame %u", fr);
      CHECK(std::memcmp(f_off.patches.data(), f_on.patches.data(), f_off.patches.size() * sizeof(cap::BankPatch)) == 0, "patches differ at frame %u", fr);
      CHECK(std::memcmp(f_off.fulls.data(), f_on.fulls.data(), f_off.fulls.size() * sizeof(cap::ConstantBank)) == 0, "fulls differ at frame %u", fr);
    }
    CHECK(f_off.draws == f_on.draws, "draw indices differ at frame %u", fr);
  }
  std::printf("  seed %u marks %d density %u same %u%% writes %u%% dev %u%%: draws %llu, banks checked %llu; ON blocks: old plan %llu, built %llu, compared %llu, skipped unchanged %llu, no-compare patches %llu, refresh compares %llu, patches %llu; "
              "emits %llu (old plan %llu)\n",
              cfg.seed, cfg.marks_on, cfg.lit_density, cfg.same_table_pct, cfg.write_pct, cfg.dev_change_pct, (unsigned long long)tot.draws, (unsigned long long)tot.banks_checked,
              (unsigned long long)on.ctr.blocks_touch, (unsigned long long)on.ctr.blocks_built, (unsigned long long)on.ctr.blocks_compared, (unsigned long long)on.ctr.blocks_skipped_same,
              (unsigned long long)on.ctr.blocks_skipped_known, (unsigned long long)on.ctr.refresh_blocks, (unsigned long long)on.ctr.patches, (unsigned long long)on.ctr.emits,
              (unsigned long long)on.ctr.emits_old_plan);
  if (g_plant == 0 && cfg.flip == 0) {  // the lever must actually take blocks, or the test proves nothing (a lever that does nothing passes every equality)
    CHECK(on.ctr.blocks_skipped_same > 0 && on.ctr.blocks_skipped_known > 0 && on.ctr.blocks_compared < on.ctr.blocks_touch, "the lever took nothing in this scenario");
  }
  return tot;
}

// ---- unit checks of the pieces -----------------------------------------------------------------------------------------------------------------------
static void TestPieces() {
  uint8_t a[64], b[64];
  for (int i = 0; i < 64; ++i) a[i] = b[i] = uint8_t(i * 7 + 1);
  CHECK(bd::Equal64(a, b), "equal blocks");
  for (int i = 0; i < 64; ++i) {  // a flip of any single byte is seen
    b[i] ^= 0x01;
    CHECK(!bd::Equal64(a, b), "flip at byte %d not seen", i);
    b[i] ^= 0x01;
  }
  // plan algebra (exact, switch on)
  bd::Plan p = bd::PlanEmit(true, 0b0011, 0b0100, 0b0100, true, true);   // lits same: lit block 2 not dirty is skipped; dirty blocks 0,1 are pure
  CHECK(p.visit == 0b0011 && p.known_diff == 0b0011, "same lits, pure dirty only");
  p = bd::PlanEmit(true, 0b0110, 0b0100, 0b0100, true, true);            // dirty block 2 under a literal: compared; block 1 pure
  CHECK(p.visit == 0b0110 && p.known_diff == 0b0010, "dirty literal block is compared");
  p = bd::PlanEmit(true, 0b0001, 0b0100, 0b1000, false, true);           // lits changed: every literal block (now and last) is visited and compared
  CHECK(p.visit == 0b1101 && p.known_diff == 0b0001, "changed lits: all literal blocks");
  p = bd::PlanEmit(true, 0b0001, 0b0100, 0b0100, true, false);           // not exact: the old set
  CHECK(p.visit == 0b0101 && p.known_diff == 0 && p.old_plan, "not exact: old plan");
  p = bd::PlanEmit(false, 0b0001, 0b0100, 0b1000, true, true);           // off: the old set
  CHECK(p.visit == 0b1101 && p.known_diff == 0 && p.old_plan, "off: old plan");
}

static void Bench() {
  // a Bryan-like draw stream: 2 stages, ~2.5 marked blocks a stage a draw of which ~60 % change, 92 % of draws repeat the last shader pair (R13 / S100: "92% took the last draw's literal tables")
  struct Ev {
    uint8_t n;
    uint8_t blk[2][8];
    uint8_t nb[2];
    uint8_t table;
    bool change[2][8];
  };
  Rng r(77);
  Device dev;
  for (int s = 0; s < 2; ++s)
    for (uint32_t i = 0; i < 1024; ++i) { const uint32_t be = Bswap(r.Next()); std::memcpy(dev.raw[s] + 4 * i, &be, 4); }
  std::vector<LitTable> tables;
  for (int i = 0; i < 8; ++i) tables.push_back(MakeTable(r, 24));
  const uint32_t kDraws = 400000, kFrame = 2000;
  Device after[2];
  after[0] = after[1] = dev;
  double ns[2] = {0, 0}, ns_none = 0;
  uint64_t checksum = 0;
  for (int pass = 0; pass < 3; ++pass) {  // pass 0 = OFF, 1 = ON, 2 = the writes alone (subtracted)
    Rng q(5);
    Device d = dev;
    Model m;
    Frame f;
    m.on = pass == 1;
    m.frame = &f;
    uint64_t table = 0, gen = 1;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kDraws; ++i) {
      if (i % kFrame == 0) { ++gen; f = Frame(); }
      for (int s = 0; s < 2; ++s) {
        const uint32_t n = 1 + q.Below(4);
        for (uint32_t k = 0; k < n; ++k) {
          const uint32_t b = q.Below(64);
          m.marked[s] |= 1ull << b;
          if (q.Chance(60)) { const uint32_t be = Bswap(q.Next()); std::memcpy(d.raw[s] + 64 * b + 4 * q.Below(16), &be, 4); }
        }
      }
      if (!q.Chance(92)) table = q.Below(8);
      if (pass < 2) {
        PushBanks(m, &d, tables, table, gen);
        checksum += m.vs_index + m.ps_index;
      }
    }
    const double e = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kDraws;
    if (pass < 2) ns[pass] = e; else ns_none = e;
  }
  std::printf("BENCH ns a draw (model glue + the writes; this PC, -O2): OFF %.1f, ON %.1f, the writes alone %.1f -> PushBanks work OFF %.1f, ON %.1f, saved %.1f ns a draw (%.0f %%) [checksum %llu]\n",
              ns[0], ns[1], ns_none, ns[0] - ns_none, ns[1] - ns_none, ns[0] - ns[1], 100.0 * (ns[0] - ns[1]) / std::max(1.0, ns[0] - ns_none), (unsigned long long)checksum);
  // the compare alone: 64-byte blocks, memcmp vs Equal64, 6 blocks a draw
  alignas(64) static uint8_t A[4096], B[4096];
  for (int i = 0; i < 4096; ++i) A[i] = B[i] = uint8_t(i * 31);
  uint64_t acc = 0;
  const uint32_t kN = 20000000;
  auto t0 = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < kN; ++i) { const uint32_t b = (i * 7) & 63; acc += std::memcmp(A + 64 * b, B + 64 * b, 64) != 0; B[64 * ((i * 5) & 63)] ^= (i & 1024) ? 1 : 0; }
  const double t_memcmp = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kN;
  t0 = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < kN; ++i) { const uint32_t b = (i * 7) & 63; acc += !bd::Equal64(A + 64 * b, B + 64 * b); B[64 * ((i * 5) & 63)] ^= (i & 1024) ? 1 : 0; }
  const double t_eq = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kN;
  std::printf("BENCH one 64-byte block compare (hot in L1): memcmp %.2f ns, Equal64 %.2f ns [acc %llu]\n", t_memcmp, t_eq, (unsigned long long)acc);
}

int main(int argc, char** argv) {
  bool bench = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "bench") == 0) bench = true;
    else g_plant = std::atoi(argv[i]);
  }
  TestPieces();
  uint64_t banks = 0;
  Config c;
  // frames, block boundaries, equal writes, partial writes, device changes, literals: many seeds and mixes; marks on and off (every draw a full compare)
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    c.seed = seed;
    c.marks_on = true;
    banks += RunScenario(c).banks_checked;
  }
  c.seed = 7; c.marks_on = false;
  banks += RunScenario(c).banks_checked;
  c.marks_on = true;
  c.seed = 8; c.lit_density = 40; c.same_table_pct = 50; banks += RunScenario(c).banks_checked;     // heavy literals, tables changing
  c.seed = 9; c.lit_density = 2; c.same_table_pct = 99; c.write_pct = 90; banks += RunScenario(c).banks_checked;  // few literals, many writes
  c.seed = 10; c.lit_density = 12; c.same_table_pct = 90; c.write_pct = 60; c.dev_change_pct = 20; banks += RunScenario(c).banks_checked;  // device changes
  c.seed = 12; c.flip = 1; banks += RunScenario(c).banks_checked;   // F3: the switch flips per frame
  c.seed = 13; c.flip = 2; banks += RunScenario(c).banks_checked;   // F3: the switch flips per draw
  c.seed = 14; c.flip = 2; c.dev_change_pct = 20; banks += RunScenario(c).banks_checked;
  c.flip = 0;
  c.seed = 11; c.draws_per_frame = 3; c.frames = 4000; c.dev_change_pct = 5; banks += RunScenario(c).banks_checked;  // frame boundaries every 3 draws
  std::printf("%s: %llu banks checked against the oracle, %d failures (plant %d)\n", g_fail ? "FAIL" : "PASS", (unsigned long long)banks, g_fail, g_plant);
  if (bench) Bench();
  return g_fail ? 1 : 0;
}
