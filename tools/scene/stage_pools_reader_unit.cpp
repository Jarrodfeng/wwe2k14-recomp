// Lane L2 unit (offline, no game): the entrance page's pool / spot rows derived from the record's light object (stage_pool_words.h) and the entering-slot reader (inputs_core.h ReadEntranceSlot), against LITERAL offsets
// taken from the listings, never the code's own constants. stage_pools_reader_unit [--plant N]
//   1 = the fake game copies the rig's TARGET position row (233 + t) into the light object instead of the CURRENT one (267 + t): the mid-fade "current" check must fail.
//   2 = the fake game copies the tint block's +0 (from) instead of +32 (current): the mid-fade tint check must fail.
//   3 = the entering-slot reader's guest has slot 1's step byte as -1 and slot 0 entering: still slot 0 -> the "second slot enters" check is the one that catches a reader fixed on slot 0 (the plant makes the guest say slot 1): must fail.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/stage_pool_words.h"
#include "../../src/renderer_scene/stage_look.h"
#include "../../src/scene_live/inputs_core.h"

using namespace rdr_scene;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
static void PutF(uint32_t* words, uint32_t w, float f) { std::memcpy(&words[w], &f, 4); }
static float GetF(const uint32_t* words, uint32_t w) { float f; std::memcpy(&f, &words[w], 4); return f; }

// ---- the fake game: sub_827F8808's unrolled tail, written from the listing with literal numbers (generated\default\wwe2k14_recomp.34.cpp):
//   light object R + 416 + 16 (t - 7) <- rig row 16 * (267 + t);  R + 576 + ... <- rig row 16 * (216 + t);  R + 736 + 16 (t - 7) <- the rig block at 4816 + 80 t + 32.
// The rig object is a byte array: rows 233 (target) / 250 (from) / 267 (current) and the 80-byte blocks (+0 from, +16 target, +32 current) differ (a fade in progress).
struct Rig {
  std::vector<uint8_t> b = std::vector<uint8_t>(6400, 0);
  void F(uint32_t at, float v) { std::memcpy(&b[at], &v, 4); }
  float G(uint32_t at) const { float v; std::memcpy(&v, &b[at], 4); return v; }
};
static void Copy8808(const Rig& rig, uint32_t* block224, int plant) {
  for (uint32_t t = 7; t <= 16; ++t) {
    const uint32_t i = t - 7;
    const uint32_t pos_row = plant == 1 ? 233 + t : 267 + t;                       // the game: 267 + t
    for (uint32_t c = 0; c < 4; ++c) PutF(block224, (416 + 16 * i) / 4 + c, rig.G(16 * pos_row + 4 * c));
    for (uint32_t c = 0; c < 4; ++c) PutF(block224, (576 + 16 * i) / 4 + c, rig.G(16 * (216 + t) + 4 * c));
    const uint32_t tint_at = 4816 + 80 * t + (plant == 2 ? 0 : 32);                // the game: +32
    for (uint32_t c = 0; c < 4; ++c) PutF(block224, (736 + 16 * i) / 4 + c, rig.G(tint_at + 4 * c));
  }
}

// ---- the fake guest of the entering-slot reader: the character table 0x836F38D0, [char + 28] = the act record, whose first word is act << 16 | step << 8
static std::map<uint32_t, uint8_t> g_mem;
static void PutW(uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) g_mem[a + uint32_t(i)] = uint8_t(v >> (24 - 8 * i)); }
static bool Read(uint32_t guest, void* dst, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    auto it = g_mem.find(guest + uint32_t(i));
    if (it == g_mem.end()) return false;
    static_cast<uint8_t*>(dst)[i] = it->second;
  }
  return true;
}
static void Slot(uint32_t n, uint32_t act_word) {
  const uint32_t ch = 0xA0100000 + 0x1000 * n, ar = 0xA0200000 + 0x100 * n;
  PutW(0x836F38D0 + 4 * n, ch), PutW(ch + 28, ar), PutW(ar, act_word);
}

int main(int argc, char** argv) {
  int plant = 0;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") plant = std::atoi(argv[i + 1]);

  // 1. a mid-fade layout: current != target != from on every track
  Rig rig;
  for (uint32_t t = 7; t <= 16; ++t) {
    const float cx = -460.f - 10.f * float(t), cz = -1400.f + 7.f * float(t);
    rig.F(16 * (267 + t), cx), rig.F(16 * (267 + t) + 8, cz), rig.F(16 * (267 + t) + 12, 1.f);                    // current
    rig.F(16 * (233 + t), cx + 100.f), rig.F(16 * (233 + t) + 8, cz + 100.f), rig.F(16 * (233 + t) + 12, 1.f);    // target
    rig.F(16 * (250 + t), cx - 100.f), rig.F(16 * (250 + t) + 8, cz - 100.f), rig.F(16 * (250 + t) + 12, 1.f);    // from
    const float cur = 0.30f + 0.01f * float(t);
    for (uint32_t c = 0; c < 3; ++c) {
      rig.F(4816 + 80 * t + 0 + 4 * c, cur - 0.2f);    // from  (+0)
      rig.F(4816 + 80 * t + 16 + 4 * c, cur + 0.2f);   // target (+16)
      rig.F(4816 + 80 * t + 32 + 4 * c, cur);          // current (+32)
    }
  }
  uint32_t block[224] = {};
  Copy8808(rig, block, plant);
  StageLookRow rows[stage_pool_words::kMaxRows];
  uint32_t n = 0;
  Check(stage_pool_words::Derive(block, rows, n) && n == 9, "9 rows from a mid-fade block: the spot and the 8 pools");
  bool pos = n == 9, tint = n == 9;
  for (uint32_t k = 0; pos && k < 8; ++k) {   // rows[0] = the spot (t = 7), rows[1 + k] = pool t = 9 + k
    const uint32_t t = 9 + k;
    pos = pos && rows[1 + k].kind == kSlPool && std::fabs(rows[1 + k].dir[0] - (-460.f - 10.f * float(t))) < 1e-2f && std::fabs(rows[1 + k].dir[1] - (-1400.f + 7.f * float(t))) < 1e-2f;
    tint = tint && std::fabs(rows[1 + k].col[1] - (0.30f + 0.01f * float(t))) < 1e-4f;
  }
  Check(pos, "pool positions are the CURRENT row (267 + t), not the target (233) or the from (250) endpoints");
  Check(tint && n == 9 && rows[0].kind == kSlSpot && (rows[0].flags & kSlfAtEntrant) && std::fabs(rows[0].col[0] - 0.37f) < 1e-4f, "tints are the block's +32 (current), not +0 (from) or +16 (target); the spot row takes t = 7's");
  // literal word positions: pool t = 9 position x is at R + 416 + 32 = word 112, its tint at R + 736 + 32 = word 192
  Check(std::fabs(GetF(block, 112) - (-460.f - 90.f)) < 1e-2f && std::fabs(GetF(block, 192) - 0.39f) < 1e-4f, "literal words: pool t = 9 x at word 112 (R + 448), tint at word 192 (R + 768)");

  // 2. zero tint: the spot at step 0 (tint 0) is no row; an all-zero (unbound) block makes no rows and is not a refusal
  uint32_t z[224];
  std::memcpy(z, block, sizeof z);
  for (uint32_t c = 0; c < 4; ++c) PutF(z, 184 + c, 0.f);
  Check(stage_pool_words::Derive(z, rows, n) && n == 8 && rows[0].kind == kSlPool, "a spot tint of 0 (the entrance's first step) makes no spot row: 8 pool rows");
  uint32_t zero[224] = {};
  Check(stage_pool_words::Derive(zero, rows, n) && n == 0, "an all-zero block (tracks not bound / step 0) makes no rows and is not a refusal (unbound and zero are not told apart: both draw nothing)");

  // 3. non-finite / absurd values are refused
  uint32_t bad[224];
  std::memcpy(bad, block, sizeof bad);
  PutF(bad, 184 + 8, std::nanf(""));
  Check(!stage_pool_words::Derive(bad, rows, n) && n == 0, "a NaN tint is refused");
  std::memcpy(bad, block, sizeof bad);
  PutF(bad, 112, INFINITY);
  Check(!stage_pool_words::Derive(bad, rows, n), "an infinite position is refused");
  std::memcpy(bad, block, sizeof bad);
  PutF(bad, 114, 1.0e7f);
  Check(!stage_pool_words::Derive(bad, rows, n), "an absurd position (1e7) is refused");

  // 4. the entering slot
  g_mem.clear();
  for (uint32_t s = 0; s < 6; ++s) Slot(s, 0x00000000);
  Slot(1, 0x61A80500u);                      // slot 1 enters (act 25000 = 0x61A8, step 5)
  if (plant == 3) Slot(1, 0x61A8FF00u);      // the guest says slot 1's step is -1 (the scene resets): slot 1 is NOT entering any more
  uint32_t slot = 9;
  const bool one = scene_inputs::ReadEntranceSlot(Read, slot);
  Check(one && slot == 1, "slot 1 entering (act 0x61A8, step 5), slot 0 not: the entrant is slot 1");
  Slot(0, 0x61A80000u);                      // slot 0 also in its entrance act (step 0): two entering = ambiguous = refused
  Check(!scene_inputs::ReadEntranceSlot(Read, slot), "two slots in the entrance act: refused (no unique entrant: the stamp is refused, the frame walks)");
  Slot(0, 0x00000000), Slot(1, 0x61A8FF00u);
  Check(!scene_inputs::ReadEntranceSlot(Read, slot), "step byte -1 (the scene resets): no entering slot");
  g_mem.clear();
  Check(!scene_inputs::ReadEntranceSlot(Read, slot), "unreadable character table: no entering slot");

  // 5. the consumer: Pools() derives the rows from the record's light object and puts the spot on the ENTERING slot's root (node 2), not always slot 0
  SceneTables t;
  LightObjectRow m;
  m.kind = kLoManager, m.flags = kLoBlockRead, m.aux0 = 1, m.aux1 = 3;
  for (uint32_t k = 0; k < kLmRefCount; ++k) m.words[kLmRefBase + k] = kLightNoRow;
  m.words[kLmRefBase + kLightMaxSets + 2] = 1;
  m.words[kLmSetInfoBase + 0] = 121u | (2u << 16);   // one arena set (121) remapped to pool 1 (remap + 1 = 2): the game's entrance marker; the entrance row carries no entrance flag
  t.light_objects.push_back(m);
  LightObjectRow pool;
  pool.kind = kLoPool, pool.index = 2, pool.flags = kLoBlockRead;
  std::memcpy(pool.words, block, sizeof block);
  t.light_objects.push_back(pool);
  StageLookRow ent;
  ent.kind = kSlEntrance, ent.flags = kSlfEntrancePage, ent.rig_slot = 2;   // slot 1 enters (slot + 1); NOT kSlfEntrance: the entrance comes from the remap below (review N1)
  t.stage_look.push_back(ent);
  for (uint32_t s = 0; s < 2; ++s) {   // two actors; each skeleton's node 2 at a different place
    Object o;
    o.id = t.AddString("actor:slot:" + std::to_string(s));
    o.bones_first = uint32_t(t.bones.size()), o.bones_count = 3;
    t.objects.push_back(o);
    t.bones.resize(t.bones.size() + 3);
    std::memset(&t.bones[o.bones_first], 0, 3 * sizeof(Mat4));
    t.bones[o.bones_first + 2].m[12] = s ? 440.f : -440.f, t.bones[o.bones_first + 2].m[14] = s ? 2200.f : 1100.f;
  }
  // an entrance needs an arena set remap for stage_look::Entrance; the entrance ROW is enough here (flags bit 0)
  std::vector<assets::stage_pools::Pool> pv;
  bool inc = false;
  stage_look::Pools(t, pv, &inc);
  Check(!inc && pv.size() == 9 && pv[0].disk, "the consumer derives 9 pools / spot from the light object when no explicit rows exist");
  Check(pv.size() == 9 && std::fabs(pv[0].x - 440.f) < 12.f && std::fabs(pv[0].z - 2200.f) < 12.f, "the spot is centred on the ENTERING slot's root (slot 1: 440, 2200), not on slot 0's (-440, 1100)");
  {
    SceneTables early = t;   // a unique entering slot BEFORE the rig's remap is installed (step 0 / cue 0): the entrance row alone must not switch the look (review N1)
    early.light_objects[0].words[kLmSetInfoBase + 0] = 121u;
    Check(!stage_look::Entrance(early), "the live entrance row (page flag only) does not make an entrance by itself: no remap, no entrance");
    Check(stage_look::Entrance(t), "with a remapped set the same row is an entrance");
  }
  t.light_objects[1].words[184 + 8] = 0x7FC00000u;   // a NaN in the block the consumer reads
  stage_look::Pools(t, pv, &inc);
  Check(inc && pv.empty(), "a NaN in the pool block: no rows, reported incomplete");
  std::printf("%s  stage_pools_reader_unit: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
