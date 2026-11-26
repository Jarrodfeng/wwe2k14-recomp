// props_cast_test: the offline proof of lane PM2's filter (Session 103; docs\research\s103\PMC.md 1 / 3 / 4, PMW.md, RMP.md 2): which weapon-row items the game casts into the PROPS map (the floor's shadow map,
// sub_827E7F00: render categories 24 / 26 / 41), as weapons::CastsPropsMap decides. 0 game runs, no SDK, no game memory. It runs the SHIPPED code (src\renderer_scene\weapon_plan.h).
//   props_cast_test [--plant cast_gm|as_map_b]   with a plant the run must FAIL a check: the twin exits 0 when it was caught, 1 when the check is blind
//   cast.weapon_ids      a manager weapon casts; the skip ids {6, 7, 29, 31, 32, 34, 35, 37, 104..107} do not                                   (the same per-node rule as map B)
//   cast.event_ids       an event prop casts; ids {2065, 2114, 2511, 2555, 2621, 2642} do not; ids 2064 / 2556 do NOT either (category 43: the props pass never draws it; map B does)   <- as_map_b
//   cast.stage_class     a structure piece casts only as its own CWeapon submit of the "other" class; the cage node's rows and the slot-32 rows never
//   cast.never           gm sets and blood splats never cast; a flag-A shadow row is never a caster                                                                          <- cast_gm
//   cast.unsure_ids      ids 59 / 68 / 71 / 96 are cast (category unknown: counted, as map B)
//   cpu.wanted           the posed-copy predicate: mode 1 + casts_p keeps, props map off drops                                                                         <- as_map_b
//   cast.vs_map_b        over every id of every family the two filters differ ONLY on the event ids 2064 / 2556 (the props pass has no mode gate and no other difference [PMC])
#include <cstdio>
#include <string>

#include "../../src/renderer_scene/weapon_plan.h"

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
  if (!plant.empty() && plant != "cast_gm" && plant != "as_map_b") {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }
  const bool plant_gm = plant == "cast_gm";
  const bool as_b = plant == "as_map_b";   // the planted twin: the props map uses map B's filter (ids 2064 / 2556 cast)
  const auto cast = [&](const W::Item& it, const W::WeaponCall& w) { return as_b ? W::CastsMapB(it, w, plant_gm) : W::CastsPropsMap(it, w, plant_gm); };
  const auto weapon = [&](int32_t id) { W::Item it; it.id = id; W::WeaponCall w{}; return cast(it, w); };
  const auto event = [&](int32_t id) { W::Item it; it.id = id, it.event = true; W::WeaponCall w{}; w.slot_flags = W::kCallEventProp; return cast(it, w); };
  {
    bool ok = weapon(1) && weapon(4) && weapon(103) && weapon(108) && weapon(135);
    for (int32_t id : {6, 7, 29, 31, 32, 34, 35, 37, 104, 105, 106, 107}) ok = ok && !weapon(id);
    Check(ok, "cast.weapon_ids", ok ? "" : "a skip id casts or an ordinary weapon does not");
  }
  {
    bool ok = event(2100) && event(2063) && event(2557) && !event(2064) && !event(2556);
    for (int32_t id : {2065, 2114, 2511, 2555, 2621, 2642}) ok = ok && !event(id);
    Check(ok, "cast.event_ids", ok ? "" : "an event skip id or a category-43 id casts, or an ordinary prop does not");
  }
  {
    const auto stage = [&](uint32_t flags) { W::Item it; it.stage = true, it.id = 7; W::WeaponCall w{}; w.slot_flags = flags; return cast(it, w); };
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
    const bool ok = !cast(gm, wg) && !cast(bl, wb) && !cast(sh, ws);
    Check(ok, "cast.never", ok ? "" : "a gm set, a blood splat or a shadow-pass row was a props-map caster");
  }
  {
    bool ok = true;
    for (int32_t id : {59, 68, 71, 96}) ok = ok && weapon(id) && W::IsMapBUnsureId(id);
    Check(ok, "cast.unsure_ids", ok ? "" : "ids 59 / 68 / 71 / 96 must cast and be counted unsure");
  }
  {
    int diff = 0, bad = 0;
    for (int32_t id = 0; id < 3200; ++id) {
      for (int kind = 0; kind < 2; ++kind) {
        W::Item it; it.id = id, it.event = kind == 1;
        W::WeaponCall w{}; w.slot_flags = kind == 1 ? W::kCallEventProp : 0u;
        const bool p = W::CastsPropsMap(it, w), b = W::CastsMapB(it, w);
        if (p != b) {
          ++diff;
          if (!(kind == 1 && W::IsMapBPiece43Id(id) && b && !p)) ++bad;
        }
      }
    }
    const bool ok = diff == 2 && bad == 0;   // exactly ids 2064 and 2556 as events
    Check(ok, "cast.vs_map_b", "filters differ on " + std::to_string(diff) + " (id, family) pairs, " + std::to_string(bad) + " of them not the piece-43 ids");
  }
  {   // review 1: the posed CPU copy predicate (rf_props.cpp and the rig fixture call it)
    const auto wanted = [&](bool b, bool p, bool pm_on, bool sh, int64_t word, int32_t n) { return W::PropCpuWanted(b, p, as_b ? true : pm_on, sh, word, n); };   // as_map_b plant: the props-map switch is ignored
    const bool ok = wanted(false, true, true, true, 1, 3) &&      // mode 1 with the props map: the copy IS kept (no mode gate)
                    !wanted(false, true, false, true, 4, 2) &&    // props map off: dropped
                    !wanted(true, true, true, false, 4, 2) &&     // SHADOWS off: dropped
                    wanted(true, false, false, true, 4, -1) &&    // map B, scene word 4: kept
                    !wanted(true, false, true, true, 1, 3) &&     // map B only, mode 1: dropped
                    !wanted(true, false, true, true, 1, -1) &&    // map B only, word 1 with an unknown count: dropped
                    !wanted(true, false, true, true, -1, 2) &&    // map B only, scene word unknown: dropped
                    !wanted(false, false, true, true, 4, 2);      // nothing casts: dropped
    Check(ok, "cpu.wanted", ok ? "" : "the CPU copy predicate keeps / drops a prop wrongly (mode 1 with casts_p must keep; props map off must drop)");
  }
  if (!plant.empty()) {
    const bool caught = g_fail > 0;
    std::printf("%s  planted fault %s: %s\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? "caught" : "NOT caught (the check is blind)");
    return caught ? 0 : 1;
  }
  std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
