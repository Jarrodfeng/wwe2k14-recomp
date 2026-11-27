// weapon_light_test: the offline proof of S111 WEAPONLIGHT (docs\research\s111\WEAPONLIGHT.md). 0 game runs, no SDK, no game memory.
//   weapon_light_test [--plant props_set | all_fifty]
// It runs the SHIPPED pick rdr_scene::weapons::CWeaponLightKey (src\renderer_scene\weapon_light_key.h), the one rf_props.cpp CWeaponLights calls.
// Checks (each planted twin must fail exactly its named checks; exit code 1 when any check failed):
//   wl.kinds    every id -5..300: set 100 for 51..59 / 61 / 62 / 64 / 68 / 69, set 50 otherwise (sub_825E0F60, sub_825DDC70)   <- props_set, all_fifty
//   wl.ids      a belt row (kBeltIdBase + sub-key) is id 14 -> 50; ring steps 29 / 32 / 35 -> 50; 55 / 61 -> 100; 60 -> 50        <- props_set, all_fifty
//   wl.missing  a set the record lacks gives none (the caller's older rule); the present one is taken                               <- props_set, all_fifty
//   wl.pieces   a stage row is a CWeapon piece only as an own submit with the caster bit; cage 175 + k, cell 185 + k, ladder case 23, all set 50
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../../src/renderer_scene/weapon_light_key.h"

namespace W = rdr_scene::weapons;

static int g_fail = 0;
static void Check(bool ok, const char* name, const char* why) {
  std::printf("%s  %-22s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) g_fail = 1;
}

static int Pick(int32_t kind, std::initializer_list<int32_t> sets) {
  return W::CWeaponLightKey(kind, [&](int32_t id) {
    for (int32_t s : sets)
      if (s == id) return true;
    return false;
  });
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0) W::g_plant_weapon_light = std::strcmp(argv[i + 1], "props_set") == 0 ? 1 : std::strcmp(argv[i + 1], "all_fifty") == 0 ? 2 : 0;
  const std::initializer_list<int32_t> all = {10, 31, 50, 100, 500};

  bool kinds = true;
  for (int32_t kind = -5; kind <= 300; ++kind) {
    const bool listed = (kind >= 51 && kind <= 59) || kind == 61 || kind == 62 || kind == 64 || kind == 68 || kind == 69;
    kinds = kinds && Pick(kind, all) == (listed ? 100 : 50);
  }
  Check(kinds, "wl.kinds", "ids 51..59 / 61 / 62 / 64 / 68 / 69 -> 100, every other id -> 50");

  const bool ids = W::CWeaponKindOfId(W::BeltId(7)) == 14 && W::CWeaponKindOfId(55) == 55 && Pick(W::CWeaponKindOfId(W::BeltId(7)), all) == 50 && Pick(W::CWeaponKindOfId(29), all) == 50 &&
                   Pick(W::CWeaponKindOfId(32), all) == 50 && Pick(W::CWeaponKindOfId(35), all) == 50 && Pick(W::CWeaponKindOfId(55), all) == 100 && Pick(W::CWeaponKindOfId(61), all) == 100 &&
                   Pick(W::CWeaponKindOfId(60), all) == 50;
  Check(ids, "wl.ids", "belt -> id 14 -> 50; steps 29 / 32 / 35 -> 50; 55 / 61 -> 100; 60 -> 50");

  const bool missing = Pick(5, {50}) == 50 && Pick(55, {50}) == -1 && Pick(5, {100}) == -1 && Pick(55, {100}) == 100 && Pick(5, {}) == -1;
  Check(missing, "wl.missing", "a set the record lacks -> none; the present one -> taken");

  const uint32_t own_other = W::PackStageFlags(3, true, false, W::kStageLrOther);
  bool pieces = W::IsCWeaponPieceRow(own_other | W::kCallStageCaster) && !W::IsCWeaponPieceRow(own_other) &&
                !W::IsCWeaponPieceRow(W::PackStageFlags(3, false, true, W::kStageLrCage) | W::kCallStageCaster) && !W::IsCWeaponPieceRow(W::kCallStageCaster);
  pieces = pieces && W::StagePieceKind(0, 3) == 178 && W::StagePieceKind(3, 2) == 187 && W::StagePieceKind(4, 1) == 23 && W::StagePieceKind(20, 1) == 23 && W::StagePieceKind(6, 0) == -1 && W::StagePieceKind(7, 0) == -1;
  for (uint32_t k = 0; k <= 8; ++k) pieces = pieces && W::CWeaponSet(W::StagePieceKind(0, k)) == 50 && W::CWeaponSet(W::StagePieceKind(3, k)) == 50;
  pieces = pieces && W::CWeaponSet(23) == 50 && W::CWeaponSet(-1) == 50;
  Check(pieces, "wl.pieces", "own + caster bit = a CWeapon piece; cage 175 + k / cell 185 + k / ladder case 23 -> set 50");

  std::printf("%s\n", g_fail ? "weapon_light_test: FAILED" : "weapon_light_test: all checks passed");
  return g_fail;
}
