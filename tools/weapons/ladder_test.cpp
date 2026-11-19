// ladder_test: the offline proof of lane LDR (Session 103; docs\research\s103\COV10_match_objects.md B1 / B2 / C1 / C2). 0 game runs, no SDK, no game memory.
//   ladder_test [--plant ladder_no_path | steps_skip | belt_raw | fixed_copies | light_flat]
// It runs the SHIPPED code: the stage path latch + the type-4 holder pull + per-call stage rows (src\scene_live\weapon_rows_core.h), the weapon manager's belt sub-key, the path / id tables (src\renderer_scene\weapon_rows.h),
// the plan (steps drawn from the rows, belts with their sub-key, the ladder PACH range) and the companion coverage census (src\renderer_scene\weapon_plan.h), against a fake guest.
// Checks (each planted twin must fail exactly its named check; exit code 1 when any check failed):
//   ldr.path_pach        StagePathPach: LADM -> 4, LADW -> 5, /STGL/0000..0045 -> 11..56, 0086..0090 -> 57..61; 0046..0085, 0091, LADU, LADC, /STG/CAGE, junk -> -1
//   ldr.ladder_pull      a type-4 holder pulls members only with a ladder path latched; LADU / nothing latched / a non-ladder type give none; types 0-3 / 5 / 7 ignore the latch
//   ldr.ladder_title     /STGL/0004 -> PACH 15: the rope (k 0) and the belt (k 1) of the holder are two stage rows with id kStageIdBase + 15, own-submit family, planned and counted as ladder   <- ladder_no_path
//   ldr.steps_variant    the weapon table's step ids 29 / 32 / 35 are drawn as the ids the rows name (the game's own colour), counted per variant                                          <- steps_skip
//   ldr.desk_once        the commentary desk (6 / 7) called once is ONE item each; the arena loader's fixed copies are not drawn when the rows are on (one desk in the picture)       <- fixed_copies
//   ldr.belt_subkey      the manager's id 14 is recorded as kBeltIdBase + the title's sub-key (o ? [o+8] : M ? [M+189208] : 1), planned (not skipped) and named "belt:<sub>"          <- belt_raw
//   ldr.wpbs_ids         the brawl-only ids (17, 51-58, 61, 62, 64, 68, 69) are weapon calls the plan draws and the census counts as wpbs; a normal id is not
//   ldr.coverage_line    the companion census line: recorded / planned / drawn per class
//   ldr.plan_range       a ladder PACH above 15 (56, 61) is a known stage structure for the plan, the latch and the held rows (the arrays are sized kStagePachCap)
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/weapon_draw_core.h"   // EntryOf (+ weapon_plan.h)
#include "../../src/scene_live/weapon_rows_core.h"

using namespace rdr_scene;
using namespace rdr_scene::live::weaponrows;
namespace W = rdr_scene::weapons;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-22s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

struct Guest {
  std::map<uint32_t, uint32_t> words;
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  void PutFloat(uint32_t a, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    words[a] = b;
  }
  bool ReadMats(uint32_t a, uint32_t n, std::vector<Mat4>& out) const {
    out.resize(n);
    for (uint32_t i = 0; i < n * 16; ++i) {
      auto it = words.find(a + 4 * i);
      if (it == words.end()) return false;
      std::memcpy(&reinterpret_cast<float*>(out.data())[i], &it->second, 4);
    }
    return true;
  }
  void WriteNodes(uint32_t buf, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t j = 0; j < 16; ++j) PutFloat(buf + 64 * i + 4 * j, float(seed * 1000 + i * 16 + j));
  }
  void MakeModel(uint32_t obj, uint32_t nodes) {   // a skinned model whose slot 2 is sub_82BD3AB8 and whose count is in [this+300]
    words[obj] = 0x8206AF90, words[0x8206AF90 + 8] = 0x82BD3AB8;
    words[obj + 296] = 1, words[obj + 300] = nodes;
    PutFloat(obj + 252, 1.0f);
  }
  void MakeHolder(uint32_t h, uint32_t type, uint32_t count, std::initializer_list<std::pair<uint32_t, uint32_t>> models, uint32_t loaded = 1) {
    words[W::kHolderGlobal] = h;
    words[h + W::kHolderOffLoaded] = loaded, words[h + W::kHolderOffType] = type, words[h + W::kHolderOffCount] = count;
    for (uint32_t k = 0; k < 18; ++k) words[h + W::kHolderOffModel + 4 * k] = 0;
    for (const auto& m : models) words[h + W::kHolderOffModel + 4 * m.first] = m.second;
  }
  // The weapon manager: slot i = {id, main model}; no secondary models.
  void MakeManager(uint32_t mgr, std::initializer_list<std::pair<int32_t, uint32_t>> slots) {
    words[W::kMgrGlobal] = mgr;
    words[mgr + W::kOffSlotCount] = uint32_t(slots.size());
    uint32_t i = 0;
    for (const auto& s : slots) {
      words[mgr + W::kOffSlotId + 4 * i] = uint32_t(s.first), words[mgr + W::kOffSlotMain + 4 * i] = s.second;
      for (uint32_t k = 0; k < 4; ++k) words[mgr + W::kOffSlotSecondary + 16 * i + 4 * k] = 0;
      ++i;
    }
  }
};

struct Rig {
  Guest g;
  Tracker t;
  CallResult Call(uint32_t model, uint32_t r5, uint64_t frame, bool own, float f1, uint32_t lr, uint32_t own_caller = 0) {
    return t.OnModelCall(model, r5, frame, own, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); }, f1, lr, own_caller);
  }
  bool Pull(StageView& v, int32_t ladder_pach) {
    auto rd = [&](uint32_t a, uint32_t& x) { return g.ReadWord(a, x); };
    return ReadStageHolder(rd, v, ladder_pach);
  }
};
static bool TakeFrame(Rig& r, uint64_t frame, FrameBuffer& fb) {
  while (r.t.TakeNext(fb))
    if (fb.valid && fb.frame == frame) return true;
  return false;
}
static W::View ViewOf(const FrameBuffer& fb) {
  W::View v;
  v.calls = fb.calls.data(), v.n_calls = fb.calls.size();
  v.nodes = reinterpret_cast<const float*>(fb.nodes.data()), v.n_nodes = fb.nodes.size();
  return v;
}
static int32_t PachOf(const char* s) { return W::StagePathPach(s, uint32_t(std::strlen(s))); }

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string plant_name;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant_name = argv[++i];
  static const char* const kPlants[] = {"ladder_no_path", "steps_skip", "belt_raw", "fixed_copies", "light_flat"};
  bool known = plant_name.empty();
  for (const char* k : kPlants) known = known || plant_name == k;
  if (!known) {
    std::printf("unknown plant %s\n", plant_name.c_str());
    return 2;
  }
  const uint32_t kH = 0x90100000, kRope = 0xA2000000, kBelt = 0xA2001000, kBufA = 0xB2000000, kBufB = 0xB2001000, kLrRope = 0x825DB750, kLrBelt = 0x825DFC00;
  const uint32_t kMgr = 0x90200000, kM = 0x90300000, kSettings = 0x90400000, kTitleObj = 0x90500000;
  const Plant tracker_plant = plant_name == "ladder_no_path" ? Plant::LadderNoPath : plant_name == "belt_raw" ? Plant::BeltNoSubKey : Plant::None;
  W::Plants plants;
  plants.draw_steps = plant_name != "steps_skip";

  // ---- ldr.path_pach
  {
    const bool ok = PachOf("/STG/LADM") == 4 && PachOf("/STG/LADW") == 5 && PachOf("/STGL/0000") == 11 && PachOf("/STGL/0004") == 15 && PachOf("/STGL/0045") == 56 && PachOf("/STGL/0086") == 57 && PachOf("/STGL/0090") == 61 &&
                    PachOf("/STGL/0046") == -1 && PachOf("/STGL/0085") == -1 && PachOf("/STGL/0091") == -1 && PachOf("/STG/LADU") == -1 && PachOf("/STG/LADC") == -1 && PachOf("/STG/CAGE") == -1 && PachOf("/STG/INFE") == -1 && PachOf("") == -1 &&
                    PachOf("/STGL/00x4") == -1 && PachOf("/STGL/00041") == -1 && PachOf("/STG/LADMX") == -1;
    Check(ok, "ldr.path_pach", "LADM 4, LADW 5, STGL 0004 -> 15, 0045 -> 56, 0086 -> 57, 0090 -> 61; the gaps, LADU / LADC and every other path -> -1");
    bool inv = true;
    for (int32_t p : {4, 5, 11, 40, 56, 57, 61}) inv = inv && W::StageTypeOfPach(p) == 4 && W::IsLadderPach(p);
    for (int32_t p : {0, 3, 6, 7, 8, 9, 10, 62}) inv = inv && !W::IsLadderPach(p);
    Check(inv && W::StagePachOfType(4) == -1, "ldr.ladder_ranges", "type 4 never maps by type; the ladder PACH range is 4, 5, 11..61");
  }

  // ---- ldr.ladder_pull
  {
    Rig r;
    r.g.MakeHolder(kH, 4, 4, {{0, kRope}, {1, kBelt}});
    r.g.MakeModel(kRope, 2), r.g.MakeModel(kBelt, 2);
    StageView v;
    const bool with_path = r.Pull(v, 15) && v.type == 4 && v.pach == 15 && v.members.size() == 2 && v.members[0].k == 0 && v.members[1].k == 1;
    StageView none;
    const bool no_path = !r.Pull(none, -1) && none.members.empty();   // LADU / LADC / nothing latched
    // a cage holder ignores a ladder latch; the cell too
    r.g.MakeHolder(kH, 0, 2, {{0, kRope}, {1, kBelt}});
    StageView cage;
    const bool cage_ok = r.Pull(cage, 15) && cage.pach == 0 && cage.members.size() == 2;
    r.g.MakeHolder(kH, 4, 4, {{0, kRope}, {1, kBelt}}, 0);   // the request still pending
    StageView pend;
    const bool pend_ok = !r.Pull(pend, 15);
    Check(with_path && no_path && cage_ok && pend_ok, "ldr.ladder_pull", "type 4 + PACH 15 -> 2 members (k 0 rope, k 1 belt); no latched PACH -> none; a cage ignores the latch; a pending request -> none");
  }

  // ---- ldr.ladder_title / ldr.plan_range (the whole path: hook latch -> tracker -> record rows -> plan -> census)
  {
    Rig r;
    r.t.plant = tracker_plant;
    r.g.MakeHolder(kH, 4, 4, {{0, kRope}, {1, kBelt}});
    r.g.MakeModel(kRope, 2), r.g.MakeModel(kBelt, 2);
    r.g.WriteNodes(kBufA, 2, 1), r.g.WriteNodes(kBufB, 2, 2);
    r.t.NoteStagePath("/STGL/0004", 10);   // what the raw hook on sub_82576768 hands over (the title-keyed rig)
    r.Call(kRope, kBufA, 100, true, 1.0f, kLrRope, kLrRope);    // the LDBL node's slot-13 submit (category 23, sub_825DB710)
    r.Call(kBelt, kBufB, 100, true, 1.0f, kLrBelt, kLrBelt);    // the belt CWeapon's own submit (sub_825DFA78)
    r.Call(kRope, kBufA, 101, true, 1.0f, kLrRope, kLrRope);    // closes frame 100
    FrameBuffer fb;
    const bool got = TakeFrame(r, 100, fb);
    const int32_t want_id = W::StageId(15);
    bool rows = got && fb.calls.size() == 2;
    if (rows)
      for (const W::WeaponCall& c : fb.calls) rows = rows && W::IsStageCall(c.slot_flags) && W::IsStageOwn(c.slot_flags) && c.id == want_id;
    const bool ks = rows && W::StageEntry(fb.calls[0].slot_flags) == 0 && W::StageEntry(fb.calls[1].slot_flags) == 1;
    W::Census c;
    W::Coverage cov;
    std::vector<W::Item> items;
    if (got) {
      const W::View v = ViewOf(fb);
      items = W::Plan(v, plants, c);
      W::CountRecorded(v, cov);
      W::CountPlanned(items, cov);
    }
    bool planned = items.size() == 2;
    for (const W::Item& it : items) planned = planned && it.stage && it.id == 15 && !it.event;
    Check(rows && ks && planned && c.stage.pach == 15 && cov.rec[W::kCovLadder] == 2 && cov.planned[W::kCovLadder] == 2 && cov.ladder_pach == 15, "ldr.ladder_title",
          "rows " + std::to_string(fb.calls.size()) + " ids " + (fb.calls.empty() ? std::string("-") : std::to_string(fb.calls[0].id)) + " (want " + std::to_string(want_id) + "), items " + std::to_string(items.size()) + ", census pach " + std::to_string(c.stage.pach) + ", ladder rec/planned " + std::to_string(cov.rec[W::kCovLadder]) + "/" + std::to_string(cov.planned[W::kCovLadder]));
    // a second path: the no-title rig (LADM = PACH 4) and a high title row (PACH 61)
    Rig r2;
    r2.g.MakeHolder(kH, 4, 4, {{0, kRope}, {1, kBelt}});
    r2.g.MakeModel(kRope, 2), r2.g.MakeModel(kBelt, 2);
    r2.g.WriteNodes(kBufA, 2, 1);
    bool range_ok = true;
    for (const char* path : {"/STG/LADM", "/STG/LADW", "/STGL/0045", "/STGL/0090"}) {
      r2.t.NoteStagePath(path, uint32_t(std::strlen(path)));
      const int32_t want_pach = PachOf(path);
      static uint64_t fr = 200;
      r2.Call(kRope, kBufA, fr, true, 1.0f, kLrRope, kLrRope);
      r2.Call(kRope, kBufA, fr + 1, true, 1.0f, kLrRope, kLrRope);
      FrameBuffer f2;
      if (!TakeFrame(r2, fr, f2) || f2.calls.size() != 1 || f2.calls[0].id != W::StageId(want_pach)) {
        range_ok = false;
        break;
      }
      W::Census c2;
      W::StageFamilyLatch latch;
      W::Plants p2 = plants;
      p2.latch = &latch;
      const std::vector<W::Item> it2 = W::Plan(ViewOf(f2), p2, c2);
      range_ok = range_ok && it2.size() == 1 && it2[0].stage && it2[0].id == want_pach && c2.unknown_id == 0 && latch.fam[want_pach] >= 0;
      fr += 10;
    }
    Check(range_ok, "ldr.plan_range", "LADM 4, LADW 5, STGL 56 and 61 each plan one stage item (the plan's per-PACH arrays hold PACH up to " + std::to_string(W::kStagePachCap - 1) + ")");
  }

  // ---- ldr.light_additive (S103 LDR2, LDC.md): the ladder rig's light pool / shaft (k 2 / 3) are drawn additive under key set 500; the rope (k 0) has set 500 only; the belt (k 1) and PACH 5 / 15 k 0 / 1 stay as they were
  {
    const bool planted = plant_name == "light_flat";   // the planted twin: the rule switched off (the lights stay opaque, props set)
    const auto additive = [&](int32_t pach, uint32_t k) { return !planted && W::IsLadderLightPiece(pach, k); };
    const auto key = [&](int32_t pach, uint32_t k) { return planted && k != 0 ? -1 : W::LadderLightKey(pach, k); };
    bool ok = additive(4, 2) && additive(4, 3) && !additive(4, 0) && !additive(4, 1);
    for (int32_t p : {5, 15}) ok = ok && additive(p, 2) == !planted && additive(p, 3) == !planted && !additive(p, 0) && !additive(p, 1);   // LADW / STGL carry two models today: k 2 / 3 do not occur there, the rule is the code's [LDC 4.1]
    ok = ok && !additive(3, 2) && !additive(0, 3) && !additive(7, 2) && !additive(6, 3);   // the cage / cell / Inferno structures never match
    ok = ok && key(4, 0) == 500 && key(4, 2) == 500 && key(4, 3) == 500 && key(4, 1) == -1 && key(15, 0) == 500 && key(15, 1) == -1 && key(3, 0) == -1;
    Check(ok, "ldr.light_additive", "LADM k 2 / k 3 additive; k 0 / k 1 and the cage / cell / Inferno untouched; light key set 500 for k 0 / 2 / 3, none for the belt");
  }

  // ---- the weapon manager's rows: steps variants, desk, belt, WPBS
  {
    const uint32_t kS29 = 0xC3000000, kS32 = 0xC3001000, kS35 = 0xC3002000, kDeskA = 0xC3003000, kDeskB = 0xC3004000, kBox = 0xC3005000, kTv = 0xC3006000, kChair = 0xC3007000, kBeltM = 0xC3008000;
    const uint32_t kSub = 45;   // the title's belt
    auto make_match = [&](Rig& r, std::initializer_list<std::pair<int32_t, uint32_t>> slots) {
      r.t.plant = tracker_plant;
      r.g.MakeManager(kMgr, slots);
      for (const auto& s : slots) r.g.MakeModel(s.second, 2);
      r.g.words[W::kTitleMgrGlobal] = kM, r.g.words[kM + W::kTitleMgrOffKey] = 7;                  // M's key 7 (the fallback)
      r.g.words[W::kSettingsGlobal] = kSettings, r.g.words[kSettings + W::kSettingsOffTitleObj] = kTitleObj;   // o's key wins
      r.g.words[kTitleObj + W::kTitleObjOffKey] = kSub;
      r.g.WriteNodes(kBufA, 2, 3);
    };
    // steps: the arena's byte picked variant 35 (the slot id word holds the REMAPPED id: sub_825DDAB0 -> stwx at mgr + 4 * (n + 97))
    Rig r;
    make_match(r, {{35, kS35}, {32, kS32}, {29, kS29}, {6, kDeskA}, {7, kDeskB}, {17, kBox}, {53, kTv}, {3, kChair}, {14, kBeltM}});
    for (uint32_t m : {kS35, kS32, kS29, kDeskA, kDeskB, kBox, kTv, kChair, kBeltM}) r.Call(m, kBufA, 300, true, 1.0f, 0);
    r.Call(kChair, kBufA, 301, true, 1.0f, 0);
    FrameBuffer fb;
    const bool got = TakeFrame(r, 300, fb);
    W::Census c;
    W::Coverage cov;
    std::vector<W::Item> items;
    if (got) {
      const W::View v = ViewOf(fb);
      items = W::Plan(v, plants, c);
      W::CountRecorded(v, cov);
      W::CountPlanned(items, cov);
      for (const W::Item& it : items)
        if (const int cc = W::CovClassOfItem(it); cc >= 0) ++cov.drawn[cc];   // (the applier counts drawn the same way, rf_props.cpp)
    }
    const auto count_id = [&](int32_t id) {
      size_t n = 0;
      for (const W::Item& it : items) n += it.id == id && !it.stage && !it.event;
      return n;
    };
    // steps
    const bool steps_ok = got && count_id(35) == 1 && count_id(32) == 1 && count_id(29) == 1 && cov.rec[W::kCovSteps35] == 1 && cov.drawn[W::kCovSteps35] == 1 && cov.drawn[W::kCovSteps29] == 1 && cov.drawn[W::kCovSteps32] == 1 && c.skipped_steps == 0;
    Check(steps_ok, "ldr.steps_variant", "rows name 29 / 32 / 35: items " + std::to_string(count_id(29)) + "/" + std::to_string(count_id(32)) + "/" + std::to_string(count_id(35)) + ", skipped steps " + std::to_string(c.skipped_steps) + ", drawn v35 " + std::to_string(cov.drawn[W::kCovSteps35]));
    // desk: once per call; the arena's fixed copy is out of the picture while the rows are on
    W::g_force_arena_fixed_props = plant_name == "fixed_copies";   // the twin: the diagnostic switch draws the arena fixed pair together with the rows (the old double desk); the default decides per record
    const size_t desk_items = count_id(6) + count_id(7);
    const size_t desk_in_picture = desk_items / 2 + (W::ArenaFixedPropsDrawn(9) ? 1 : 0);   // the desk is two weapon rows (6 = the desk, 7 = its top) + the arena's fixed pair of meshes when drawn
    Check(got && count_id(6) == 1 && count_id(7) == 1 && desk_in_picture == 1 && cov.rec[W::kCovDesk] == 2 && cov.drawn[W::kCovDesk] == 2, "ldr.desk_once", "desk rows " + std::to_string(desk_items) + ", fixed copies drawn " + std::string(W::ArenaFixedPropsDrawn(9) ? "yes" : "no") + " -> desks in the picture " + std::to_string(desk_in_picture));
    const bool rig_keeps = W::ArenaFixedPropsDrawn(0);   // a record with no weapon table (an offline rig): the fixed pair stays
    W::g_force_arena_fixed_props = false;
    Check(rig_keeps && !W::ArenaFixedPropsDrawn(9), "ldr.fixed_per_record", "a record with no weapon table keeps the arena's fixed pair; one with rows hides it (decided per record)");
    {   // belt sub-key: read once per belt object across frames; the section-59 rows carry it too
      std::vector<W::WeaponSlot> rows(1);
      rows[0].id = 14, rows[0].main = kBeltM;
      live::weapons::BeltKeyCache cache;
      auto rdk = [&](uint32_t a, uint32_t& v) { return r.g.ReadWord(a, v); };
      live::weapons::ApplyBeltIds(rdk, cache, rows);
      const int32_t first = rows[0].id;
      rows[0].id = 14;
      live::weapons::ApplyBeltIds(rdk, cache, rows);
      std::vector<W::WeaponSlot> none;
      live::weapons::ApplyBeltIds(rdk, cache, none);   // the belt left the slots: the entry goes
      Check(first == W::BeltId(kSub) && rows[0].id == first && cache.reads == 1 && cache.e.empty() && r.t.BeltKeyReads() == (plant_name == "belt_raw" ? 0u : 1u), "ldr.belt_cached", "section-59 row id " + std::to_string(first) + ", reads " + std::to_string(cache.reads) + " (cache), tracker reads " + std::to_string(r.t.BeltKeyReads()) + " over two frames");
    }
    // belt
    size_t belt_items = 0;
    int32_t belt_id = 0;
    for (const W::Item& it : items)
      if (W::BeltSubOfId(it.id) >= 0) ++belt_items, belt_id = it.id;
    Check(got && belt_items == 1 && belt_id == W::BeltId(kSub) && W::EntryOf(belt_id, 0) == "belt:45" && c.belts == 0 && cov.rec[W::kCovBelt] == 1 && cov.drawn[W::kCovBelt] == 1, "ldr.belt_subkey",
          "belt items " + std::to_string(belt_items) + " id " + std::to_string(belt_id) + " (want " + std::to_string(W::BeltId(kSub)) + "), entry " + W::EntryOf(belt_id, 0) + ", skipped belts " + std::to_string(c.belts));
    // WPBS
    Check(got && count_id(17) == 1 && count_id(53) == 1 && cov.rec[W::kCovWpbs] == 2 && cov.drawn[W::kCovWpbs] == 2 && W::CovClassOfWeaponId(3) == -1 && W::CovClassOfWeaponId(16) == -1 && W::CovClassOfWeaponId(69) == W::kCovWpbs, "ldr.wpbs_ids",
          "box 17 and TV 53 planned (" + std::to_string(count_id(17)) + "/" + std::to_string(count_id(53)) + "), wpbs rec/drawn " + std::to_string(cov.rec[W::kCovWpbs]) + "/" + std::to_string(cov.drawn[W::kCovWpbs]) + "; a chair (3) is not wpbs");
    // the belt's fallback sub-key chain: no o -> M's 7; no M either -> 1
    {
      Rig a;
      a.g.words[W::kTitleMgrGlobal] = kM, a.g.words[kM + W::kTitleMgrOffKey] = 7;
      auto rd = [&](uint32_t x, uint32_t& v) { return a.g.ReadWord(x, v); };
      const uint32_t s_m = BeltSubKey(rd);
      Rig b;
      auto rd2 = [&](uint32_t x, uint32_t& v) { return b.g.ReadWord(x, v); };
      const uint32_t s_none = BeltSubKey(rd2);
      Rig d;
      d.g.words[W::kTitleMgrGlobal] = kM, d.g.words[kM + W::kTitleMgrOffKey] = 0x12345;   // only the low 16 bits are the key
      auto rd3 = [&](uint32_t x, uint32_t& v) { return d.g.ReadWord(x, v); };
      Check(s_m == 7 && s_none == 1 && BeltSubKey(rd3) == 0x2345, "ldr.belt_chain", "o absent -> M's key (7); neither -> 1; the low 16 bits");
    }
    // ---- ldr.coverage_line
    const std::string line = cov.Line();
    Check(got && line.find("coverage") != std::string::npos && line.find("ladder") != std::string::npos && line.find("steps v35") != std::string::npos && line.find("wpbs 2/2/2") != std::string::npos && line.find("belt") != std::string::npos &&
              line.find("desk 2/2/2") != std::string::npos && line.find("lights additive 0") != std::string::npos,
          "ldr.coverage_line", line);
  }
  std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
