// Lane L unit (offline, no game): the stage look derived from kScLightObjects (id 32, lane A2). stage_look_unit [--plant N]
//   1 = the pool index is joined to the wrong pool (rig_wrong_slot): the arena set's tuple is the crowd's: the "set 121 gets pool 1's values" check fails.
//   2 = the entrance is not recognised (ignore_entrance): the entrance / values checks fail.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int stbi_zlib_decode_buffer(char*, int, const char*, int) { return -1; }  // pac.h's decoder hook: this unit reads no pac (the declaration pac.h needs, never called)
#include "../../src/renderer_scene/stage_look.h"

using namespace rdr_scene;
namespace sl = rdr_scene::stage_look;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

static void PutF(uint32_t* w, uint32_t at, float f) { std::memcpy(&w[at], &f, 4); }

// a block row: direction k = (0.1 (k + 1), v, 0), colour k = (v + k / 100, ...), sky = (s, s, 1), ground = (g, g, g)
static LightObjectRow Block(uint32_t kind, uint32_t index, float v, float s, float g) {
  LightObjectRow r;
  r.kind = kind, r.index = index, r.flags = kLoBlockRead, r.object = 0x1000 + index;
  for (uint32_t k = 0; k < 4; ++k)
    for (uint32_t c = 0; c < 3; ++c) {
      PutF(r.words, kLsDir + 4 * k + c, c == 0 ? 0.1f * float(k + 1) : c == 1 ? v : 0.f);
      PutF(r.words, kLsCol + 4 * k + c, v + 0.01f * float(k));
    }
  PutF(r.words, kLsBlockA + 0, s), PutF(r.words, kLsBlockA + 1, s), PutF(r.words, kLsBlockA + 2, 1.f);
  PutF(r.words, kLsBlockB + 0, g), PutF(r.words, kLsBlockB + 1, g), PutF(r.words, kLsBlockB + 2, g);
  return r;
}

// sets: (id, remap) pairs; pool j's value v = 0.1 (j + 1); `unreadable_pool` = -1 or the pool index whose object is unreadable
static SceneTables Make(const int (*sets)[2], int n, int pools, int unreadable_pool) {
  SceneTables t;
  LightObjectRow m;
  m.kind = kLoManager, m.aux0 = uint32_t(n), m.aux1 = uint32_t(pools), m.flags = kLoBlockRead;
  for (uint32_t k = 0; k < kLmRefCount; ++k) m.words[kLmRefBase + k] = kLightNoRow;
  t.light_objects.push_back(m);
  for (int j = 0; j < pools; ++j) {
    if (j == unreadable_pool) continue;
    t.light_objects.push_back(Block(kLoPool, uint32_t(j), 0.1f * float(j + 1), 0.8f + 0.01f * float(j), 0.3f));
    t.light_objects[0].words[kLmRefBase + kLightMaxSets + uint32_t(j)] = uint32_t(t.light_objects.size() - 1);
  }
  for (int i = 0; i < n; ++i) t.light_objects[0].words[kLmSetInfoBase + uint32_t(i)] = uint32_t(sets[i][0]) | (uint32_t(uint16_t(sets[i][1] + 1)) << 16);
  return t;
}

int main(int argc, char** argv) {
  int plant = 0;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") plant = std::atoi(argv[i + 1]);
  if (plant == 1) sl::PlantsRef().rig_wrong_slot = true;
  if (plant == 2) sl::PlantsRef().ignore_entrance = true;
  // arena set 121 -> pool 1, crowd set 20 -> pool 2, set 10 (the entrant) not remapped, the rig's own set 450 not remapped
  const int sets[4][2] = {{121, 1}, {20, 2}, {10, -1}, {450, -1}};
  SceneTables t = Make(sets, 4, 4, -1);
  Check(sl::Entrance(t), "an arena set with a remap makes it an entrance");
  sl::Tuple a, c;
  Check(sl::RigFor(t, 121, true, a) && std::fabs(a.col[0][0] - 0.2f) < 1e-6f && std::fabs(a.sky[0] - 0.81f) < 1e-6f, "set 121 takes pool 1's values (colour 0.2, sky 0.81)");
  Check(sl::RigFor(t, 20, true, c) && std::fabs(c.col[0][0] - 0.3f) < 1e-6f, "set 20 takes pool 2's values (colour 0.3)");
  Check(a.dir[2][0] > 0.299f && a.dir[2][0] < 0.301f, "the directions come from the block's own words (light 2 x = 0.3)");
  Check(!sl::RigFor(t, 10, true, a) && !sl::RigFor(t, 450, true, a), "sets the game does not remap (10, 450) keep their values");
  const int none[2][2] = {{121, -1}, {20, -1}};
  SceneTables m = Make(none, 2, 4, -1);
  Check(!sl::Entrance(m) && !sl::RigFor(m, 121, true, a), "no remap anywhere = the match look: nothing applied");
  SceneTables u = Make(sets, 4, 4, 1);  // pool 1's object unreadable
  bool miss = false;
  Check(!sl::RigFor(u, 121, true, a, &miss) && miss, "a remapped set whose pool object is unreadable is a MISSING input");
  std::vector<assets::LightSet> ls(2);
  ls[0].id = 121, ls[1].id = 20;
  Check(sl::MissingFor(u, ls) == 1 && sl::MissingFor(t, ls) == 0, "MissingFor counts exactly the unreadable one");
  // offline rows route (what frame_rig --look-from-stream injects)
  SceneTables o;
  StageLookRow en;
  en.kind = kSlEntrance, en.flags = kSlfEntrance;
  o.stage_look.push_back(en);
  Check(sl::Entrance(o) && sl::MissingFor(o, ls) >= 1, "an entrance row with no slot rows is a missing input");
  StageLookRow r1;
  r1.kind = kSlRigSlot, r1.rig_slot = 1;
  r1.col[0] = 0.5f;
  o.stage_look.push_back(r1);
  Check(sl::RigFor(o, 121, true, a) && std::fabs(a.col[0][0] - 0.5f) < 1e-6f, "offline rows: set 121 joins slot 1 by the game's rule (SlotForSet)");
  Check(!sl::RigFor(o, 450, true, a), "offline rows: a 4xx set is the rig's own and is not joined to a slot");
  // S103 L2: the entrance page's pool + spot rows
  {
    StageLookRow p1, sp;
    p1.kind = kSlPool, p1.dir[0] = 100.f, p1.dir[1] = -200.f, p1.dir[2] = 375.f, p1.col[0] = 0.6f, p1.col[1] = 0.5f, p1.col[2] = 0.4f;
    sp.kind = kSlSpot, sp.flags = kSlfAtEntrant, sp.col[0] = 0.3f, sp.col[1] = 0.3f, sp.col[2] = 0.3f;
    o.stage_look.push_back(p1), o.stage_look.push_back(sp);
    std::vector<assets::stage_pools::Pool> pv;
    bool inc = false;
    sl::Pools(o, pv, &inc);
    Check(pv.size() == 1 && inc && !pv[0].disk && std::fabs(pv[0].x - 100.f) < 1e-3f && std::fabs(pv[0].half - 375.f) < 1e-3f, "a pool row becomes a pool; a spot row at the entrant with no actor in the record is INCOMPLETE (a missing input)");
    // an actor with a skin palette: node 2 is the root
    Object ob;
    ob.id = o.AddString("actor:slot:0");
    ob.bones_first = 0, ob.bones_count = 4;
    o.objects.push_back(ob);
    o.bones.resize(4);
    std::memset(o.bones.data(), 0, 4 * sizeof(Mat4));
    o.bones[2].m[12] = 11.f, o.bones[2].m[14] = 2729.f;
    sl::Pools(o, pv, &inc);
    Check(pv.size() == 2 && !inc && pv[1].disk && std::fabs(pv[1].x - 22.f) < 1e-3f && std::fabs(pv[1].z - 2728.f) < 1e-3f && std::fabs(pv[1].half - 187.5f) < 1e-3f, "the spot is centred on the entrant's root (skeleton node 2: x / z, snapped to 22 units), half size 187.5");
    const uint64_t h0 = sl::PoolsHash(pv);
    o.bones[2].m[14] = 2733.f;
    std::vector<assets::stage_pools::Pool> pv3;
    sl::Pools(o, pv3, &inc);
    Check(sl::PoolsHash(pv3) == h0, "a creep of 4 units (inside the 22 unit snap) keeps the page key: no recompose");
    o.bones[2].m[14] = 2760.f;
    std::vector<assets::stage_pools::Pool> pv2;
    sl::Pools(o, pv2, &inc);
    Check(sl::PoolsHash(pv2) != h0 && h0 > 3, "the page key changes when the entrant moves (and is never the match / base keys)");
    sl::PlantsRef().ignore_pools = true;
    sl::Pools(o, pv2, &inc);
    Check(pv2.empty(), "planted: pool rows ignored");
    sl::PlantsRef().ignore_pools = false;
    sl::PlantsRef().spot_wrong = true;
    sl::Pools(o, pv2, &inc);
    Check(pv2.size() == 2 && std::fabs(pv2[1].z + 2750.f) < 1e-3f, "planted: the spot's z mirrored");
    sl::PlantsRef().spot_wrong = false;
    o.bones[2].m[14] = 2729.f;
  }
  // the record round trip of the id 45 rows
  o.objects.clear(), o.bones.clear();  // (the actor stand-in of the checks above is not a valid record object: no asset)
  o.header.guest_frame = 7;
  std::string err;
  SceneTables back;
  const bool ok = WriteRecord("stage_look_unit.rsc", o, err) && ReadRecord("stage_look_unit.rsc", back, err);
  Check(ok && back.stage_look.size() == o.stage_look.size() && std::memcmp(&back.stage_look[3], &o.stage_look[3], sizeof(StageLookRow)) == 0, "the stage look rows survive WriteRecord / ReadRecord byte for byte");
  std::remove("stage_look_unit.rsc");
  std::printf("%s  stage_look_unit: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
