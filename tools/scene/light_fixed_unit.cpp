// S103 A7: unit for src\renderer_scene\light_fixed_id.h (the light object of a ring / arena-slot model computed from the record by its FIXED set id; docs\renderer\INPUTS_A7.md). Synthetic rows to A2's manager layout
// (scene_record.h LightManagerWords). 0 game runs. Plants (each must fail its NAMED check): ring30for31 (FIXED RING PIECES), remap_ignored (FIXED REMAP), override_ignored (FIXED OVERRIDE), absent_filled (FIXED ABSENT),
// census_equal_always (FIXED CENSUS), direct_replaced (FIXED KEEP DIRECT). Usage: light_fixed_unit [--plant NAME]; exit 0 = every check passed.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/renderer_scene/light_fixed_id.h"
#include "../../src/renderer_scene/rows_inputs.h"
using namespace rdr_scene;

static int g_fail = 0;
static void Check(bool ok, const char* name) {
  if (!ok) { std::printf("FAIL %s\n", name); ++g_fail; }
}

// A record with the manager's id list `ids` (position = index), one block row per set (words[0] = a tag 1000 + position, so a block is identifiable), `pool` extra objects (tag 2000 + j), the override words and the
// remap (pool index + 1 for set position p, 0 = none).
struct Rec {
  std::vector<LightObjectRow> rows;
};
static Rec Make(const std::vector<uint32_t>& ids, uint32_t pool_n, int remap_of_pos, uint32_t remap_pool, uint32_t ov0, uint32_t ov1) {
  Rec r;
  LightObjectRow m;
  m.kind = kLoManager, m.flags = kLoBlockRead, m.aux0 = uint32_t(ids.size()), m.aux1 = pool_n, m.remap = 0;
  for (uint32_t k = 0; k < kLmRefCount; ++k) m.words[kLmRefBase + k] = kLightNoRow;
  m.words[kLmOverrideMode0] = ov0, m.words[kLmOverrideMode1] = ov1, m.words[kLmGateObject] = 0, m.words[kLmGateFlag] = 0;
  r.rows.push_back(m);
  for (uint32_t i = 0; i < ids.size(); ++i) {
    LightObjectRow b;
    b.kind = kLoSet, b.index = i, b.object = 0x10000 + 0x400 * i, b.flags = kLoBlockRead;
    b.words[0] = 1000 + i;
    r.rows.push_back(b);
    r.rows[0].words[kLmRefBase + i] = uint32_t(r.rows.size() - 1);
    uint32_t info = ids[i] & 0xFFFFu;
    if (int(i) == remap_of_pos) info |= (remap_pool + 1u) << 16;
    r.rows[0].words[kLmSetInfoBase + i] = info;
  }
  for (uint32_t j = 0; j < pool_n; ++j) {
    LightObjectRow b;
    b.kind = kLoPool, b.index = j, b.object = 0x20000 + 0x400 * j, b.flags = kLoBlockRead;
    b.words[0] = 2000 + j;
    r.rows.push_back(b);
    r.rows[0].words[kLmRefBase + kLightMaxSets + j] = uint32_t(r.rows.size() - 1);
  }
  return r;
}
static uint32_t TagOf(const Rec& r, uint32_t object) {   // the tag of the block whose source address is `object` (row.object + 48)
  for (const LightObjectRow& b : r.rows) if (b.kind != kLoManager && b.object + 48u == object) return b.words[0];
  return 0;
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == "--plant") plant = argv[i + 1];
  InstallFaults ft;
  if (plant == "remap_ignored") ft.ignore_remap = true;
  if (plant == "override_ignored") ft.ignore_override = true;
  const std::vector<uint32_t> ids = {10, 11, 20, 30, 31, 32, 100, 121};   // positions 0..7
  const std::vector<LearnedInstall> none;

  // position of an id in the manager's list (sub_827F6A40): the first match, -1 when absent
  {
    Rec r = Make(ids, 0, -1, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
    Check(FindSetPosition(r.rows[0], 30) == 3 && FindSetPosition(r.rows[0], 31) == 4 && FindSetPosition(r.rows[0], 10) == 0 && FindSetPosition(r.rows[0], 21) == -1, "FIXED POSITION");
  }
  // every class picks its constant id's block (no override, no remap)
  {
    Rec r = Make(ids, 0, -1, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
    std::vector<FixedModel> fm = {{0xA1, FixedClass::kRingCanvas, 0}, {0xA2, FixedClass::kRingPiece, 0}, {0xA3, FixedClass::kRingRope, 0}, {0xA4, FixedClass::kArena, 121}, {0xA5, FixedClass::kCrowd, 0}, {0xA6, FixedClass::kWrestler, 0}, {0xA7, FixedClass::kUnknown, 0}};
    std::vector<ByIdEntry> es;
    const ByIdCensus c = ComputeByIds(r.rows, fm, none, es, &ft, plant == "ring30for31");
    auto tag = [&](uint32_t model) { for (const ByIdEntry& e : es) if (e.model == model) return TagOf(r, e.object); return 0u; };
    Check(tag(0xA1) == 1003, "FIXED RING CANVAS");        // set 30 = position 3
    Check(tag(0xA2) == 1004, "FIXED RING PIECES");        // set 31 = position 4 (the planted twin asks 30)
    Check(tag(0xA3) == 1005, "FIXED RING ROPES");         // set 32
    Check(tag(0xA4) == 1007, "FIXED ARENA SLOT");         // l(121) = position 7
    Check(tag(0xA5) == 1002, "FIXED CROWD");              // set 20
    Check(tag(0xA6) == 0 && tag(0xA7) == 0, "FIXED SKIPS WRESTLER");   // the wrestler's embedded copy is not an id's object
    Check(c.models == 5 && c.produced == 5, "FIXED COUNTS");
  }
  // the mode-1 override word replaces the index for EVERY mode-1 install (sub_826203E8 0x82620410-0x8262043C): all take that position's block
  {
    Rec r = Make(ids, 0, -1, 0, 0xFFFFFFFFu, 6);   // position 6 = id 100
    std::vector<FixedModel> fm = {{0xA1, FixedClass::kRingCanvas, 0}, {0xA4, FixedClass::kArena, 121}};
    std::vector<ByIdEntry> es;
    ComputeByIds(r.rows, fm, none, es, &ft);
    Check(es.size() == 2 && TagOf(r, es[0].object) == 1006 && TagOf(r, es[1].object) == 1006, "FIXED OVERRIDE");
  }
  // the pool remap (mgr+120): the set's position maps to a pool object (sub_827F6828)
  {
    Rec r = Make(ids, 2, 4, 1, 0xFFFFFFFFu, 0xFFFFFFFFu);   // position 4 (id 31) -> pool 1
    std::vector<FixedModel> fm = {{0xA2, FixedClass::kRingPiece, 0}, {0xA1, FixedClass::kRingCanvas, 0}};
    std::vector<ByIdEntry> es;
    ComputeByIds(r.rows, fm, none, es, &ft);
    Check(es.size() == 2 && TagOf(r, es[0].object) == 2001 && TagOf(r, es[1].object) == 1003, "FIXED REMAP");
  }
  // an absent id installs nothing (sub_827F6A40 = -1): no entry, counted; the plant fills it with set position 0
  {
    Rec r = Make(ids, 0, -1, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
    std::vector<FixedModel> fm = {{0xA4, FixedClass::kArena, 777}};
    std::vector<ByIdEntry> es;
    ByIdCensus c = ComputeByIds(r.rows, fm, none, es, &ft);
    if (plant == "absent_filled") { c.absent_id = 0; es.push_back(ByIdEntry{0xA4, 777, 0, 1, r.rows[1].object + 48u, FixedClass::kArena}); }
    Check(es.empty() && c.absent_id == 1, "FIXED ABSENT");
  }
  // the census against the walk's learned table
  {
    Rec r = Make(ids, 0, -1, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
    std::vector<FixedModel> fm = {{0xA1, FixedClass::kRingCanvas, 0}, {0xA2, FixedClass::kRingPiece, 0}, {0xA3, FixedClass::kRingRope, 0}, {0xA4, FixedClass::kArena, 121}, {0xA8, FixedClass::kArena, 100}};
    const uint32_t obj31 = 0x10000 + 0x400 * 4 + 48u;
    std::vector<LearnedInstall> ln = {{0xA1, 0, 3, 1}, {0xA2, 0, 3, 1} /*differs: asked 3, the rule says 4*/, {0xA3, 0x99999, kFixedNoIndex, kFixedNoIndex} /*direct, another object*/, {0xA8, 0, 6, 1}};
    (void)obj31;
    std::vector<ByIdEntry> es;
    const ByIdCensus c = ComputeByIds(r.rows, fm, ln, es, &ft);
    const bool ok = c.equal == 2 && c.differ == 1 && c.direct == 1 && c.unlearned == 1;   // 0xA1 and 0xA8 equal, 0xA2 differs, 0xA3 direct, 0xA4 unlearned
    Check(plant == "census_equal_always" ? false : ok, "FIXED CENSUS");
  }
  // a learned DIRECT install of another object (the ropes' modified copy) is kept, never replaced; a learned (index, mode) install or nothing learned takes the computed entry
  {
    ByIdEntry e;
    e.model = 0xA3, e.object = 0x5000;
    const LearnedInstall direct_other{0xA3, 0x9000, kFixedNoIndex, kFixedNoIndex}, direct_same{0xA3, 0x5000, kFixedNoIndex, kFixedNoIndex}, indexed{0xA3, 0x9000, 5, 1};
    const bool pl = plant == "direct_replaced";
    Check(KeepLearned(&direct_other, e, pl) && !KeepLearned(&direct_same, e, pl) && !KeepLearned(&indexed, e, pl) && !KeepLearned(nullptr, e, pl), "FIXED KEEP DIRECT");
  }
  if (g_fail) { std::printf("light_fixed_unit: %d check(s) FAILED\n", g_fail); return 1; }
  std::printf("light_fixed_unit: all checks passed\n");
  return 0;
}
