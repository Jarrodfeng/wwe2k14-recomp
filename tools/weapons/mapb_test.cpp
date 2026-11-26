// mapb_test: the offline proof of lane MBC's filter (Session 103; docs\research\s103\RMP.md 2, PMC.md 4): which weapon-row items the game casts into the wrestlers' shadow map (map B, mode 0), and the phase row's
// participant / ring-gate words. 0 game runs, no SDK, no game memory. It runs the SHIPPED code (src\renderer_scene\weapon_plan.h CastsMapB, scene_record.h PhaseRowParticipants / PhaseRowRingGate).
//   mapb_test [--plant cast_gm]      with a plant the run must FAIL a check: the twin exits 0 when it was caught, 1 when the check is blind
//   cast.weapon_ids      a manager weapon casts; the skip ids {6, 7, 29, 31, 32, 34, 35, 37, 104..107} do not; id 135 casts (its word test is outside flag A)
//   cast.event_ids       an event prop casts; ids {2065, 2114, 2511, 2555, 2621, 2642} do not (2064 / 2556 cast: category 43 casts unless the scene word is 1)
//   cast.stage_class     a structure piece casts only as its own CWeapon submit of the "other" class; the cage node's rows and the slot-32 rows never
//   cast.never           gm sets and blood splats never cast; a flag-A shadow row is never a caster                                                              <- cast_gm
//   cast.unsure_ids      ids 59 / 68 / 71 / 96 are cast (category unknown: counted)
//   phase.row_words      the phase row's participant count and ring gate word round-trip; an older row (rig_slot 0, dir[0] 0) reads as unknown (-1)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/prop_cache_sweep.h"
#include "../../src/renderer_scene/rows_stage_look.h"
#include "../../src/renderer_scene/weapon_plan.h"
#include <memory>

using namespace rdr_scene;
namespace W = rdr_scene::weapons;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-20s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  if (!plant.empty() && plant != "cast_gm" && plant != "no_sweep") {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }
  const bool plant_gm = plant == "cast_gm";
  const auto weapon = [&](int32_t id) { W::Item it; it.id = id; W::WeaponCall w{}; return W::CastsMapB(it, w, plant_gm); };
  const auto event = [&](int32_t id) { W::Item it; it.id = id, it.event = true; W::WeaponCall w{}; w.slot_flags = W::kCallEventProp; return W::CastsMapB(it, w, plant_gm); };
  {
    bool ok = weapon(1) && weapon(4) && weapon(103) && weapon(108);
    for (int32_t id : {6, 7, 29, 31, 32, 34, 35, 37, 104, 105, 106, 107}) ok = ok && !weapon(id);
    ok = ok && weapon(135);
    Check(ok, "cast.weapon_ids", ok ? "" : "a skip id casts or an ordinary weapon does not");
  }
  {
    bool ok = event(2100) && event(2064) && event(2556) && W::IsMapBPiece43Id(2064) && W::IsMapBPiece43Id(2556) && !W::IsMapBPiece43Id(2100);
    for (int32_t id : {2065, 2114, 2511, 2555, 2621, 2642}) ok = ok && !event(id);
    Check(ok, "cast.event_ids", ok ? "" : "an event skip id casts or an ordinary prop does not");
  }
  {
    const auto stage = [&](uint32_t flags) { W::Item it; it.stage = true, it.id = 7; W::WeaponCall w{}; w.slot_flags = flags; return W::CastsMapB(it, w, plant_gm); };
    const uint32_t own_other = W::kCallStageOwn | W::kCallStageStruct | W::kCallStageCaster | (W::kStageLrOther << W::kStageLrShift);   // S111 CHAMBERSHADOW: a CWeapon piece's own row carries the recorder's caster bit
    const uint32_t own_cage = W::kCallStageOwn | W::kCallStageStruct | (W::kStageLrCage << W::kStageLrShift);
    const uint32_t slot32 = W::kCallStageStruct;
    const bool ok = stage(own_other) && !stage(own_cage) && !stage(slot32);
    Check(ok, "cast.stage_class", ok ? "" : "own/other must cast; the cage node and slot-32 rows must not");
  }
  {
    W::Item gm; gm.gm = true, gm.id = 5;
    W::WeaponCall wg{}; wg.slot_flags = W::kCallGmSet;
    W::Item bl; bl.blood = true, bl.id = 3;
    W::WeaponCall wb{}; wb.slot_flags = W::kCallBloodSplat;
    W::Item sh; sh.id = 1;
    W::WeaponCall ws{}; ws.slot_flags = W::kCallShadowPass;
    const bool ok = !W::CastsMapB(gm, wg, plant_gm) && !W::CastsMapB(bl, wb, plant_gm) && !W::CastsMapB(sh, ws, plant_gm);
    Check(ok, "cast.never", ok ? "" : "a gm set, a blood splat or a shadow-pass row was a map B caster");
  }
  {
    bool ok = true;
    for (int32_t id : {59, 68, 71, 96}) ok = ok && weapon(id) && W::IsMapBUnsureId(id);
    Check(ok, "cast.unsure_ids", ok ? "" : "ids 59 / 68 / 71 / 96 must cast and be counted unsure");
  }
  {
    StageLookRow r;
    r.kind = kSlPhase, r.flags = 1, r.rig_slot = 4, r.dir[0] = 2.f;
    StageLookRow old;
    old.kind = kSlPhase, old.flags = 4;
    StageLookRow other;
    other.kind = kSlEntrance, other.rig_slot = 3, other.dir[0] = 5.f;
    const bool ok = PhaseRowParticipants(r) == 3 && PhaseRowRingGate(r) == 1 && PhaseRowParticipants(old) == -1 && PhaseRowRingGate(old) == -1 && PhaseRowParticipants(other) == -1 && PhaseRowRingGate(other) == -1;
    Check(ok, "phase.row_words", ok ? "" : "the phase row's participant count / ring gate did not round-trip (or another kind's rig_slot was read)");
  }
  {   // review 3: the cache stays bounded over many entrances (every entrance brings fresh model ids: ids are never reused)
    struct E { uint64_t last_used = 0; };
    std::unordered_map<uint32_t, std::unique_ptr<E>> cache;
    size_t peak = 0, retired = 0;
    const uint64_t max_age = plant == "no_sweep" ? ~0ull : kPropCasterMaxAge;
    uint32_t next_id = 1;
    for (uint64_t frame = 1; frame <= 20000; ++frame) {
      if (frame % 300 == 1) {   // an entrance: 6 new props, in use for 200 frames
        for (int k = 0; k < 6; ++k) cache.emplace(next_id++, std::make_unique<E>());
      }
      for (auto& kv : cache)
        if (frame % 300 >= 1 && frame % 300 <= 200 && kv.first + 6 > next_id) kv.second->last_used = frame;   // only the newest six are asked for
      retired += SweepStaleProps(cache, frame, max_age, [](E&) {});
      peak = std::max(peak, cache.size());
    }
    Check(peak <= 12 && retired > 0, "cache.bounded", "peak " + std::to_string(peak) + " entries over 66 entrances, retired " + std::to_string(retired));
  }
  if (!plant.empty()) {
    const bool caught = g_fail > 0;
    std::printf("%s  planted fault %s: %s\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? "caught" : "NOT caught (the check is blind)");
    return caught ? 0 : 1;
  }
  std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
