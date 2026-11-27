// S111 WEAPONLIGHT (docs\research\s111\WEAPONLIGHT.md): the light set a weapon-manager CWeapon draws under, by id. Pure header (weapon_rows.h only).
// Reader: rf_props.cpp CWeaponLights. Unit: tools\weapons\weapon_light_test.cpp (tools\weapons\weapon_light_selftest.py).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C]: the CWeapon constructor sub_825E0F60 (vtable 0x82020108; its one caller sub_825E2DE0, whose desc +0 is the id the weapon maker was asked for = the manager slot id) looks up set 100
// when sub_825DDC70(id) is 1 (ids 51..59, 61, 62, 64, 68, 69) and set 50 otherwise in the light manager [0x83924778], keeping the index at +1732. Its update sub_825E3AA0 (vtable 0x820200E0 slot 1)
// calls slot 20 sub_825DE7F0 every update: a copy of that set into the weapon's own light object +1744 (a set the manager lacks copies nothing: the object keeps the constructor's defaults [U values]).
// Its draw node sub_825DFA78 installs that object (sub_82620488 with node + 544) before the slot-13 submit. So every manager weapon, the belt (id 14), the ring steps and the structure pieces built
// as CWeapons (cage ids 175 + k, cell 185 + k, the ladder's case 23: sub_824D6C68) draw under set 50 or 100 by id.
#pragma once
#include <cstdint>

#include "weapon_rows.h"

namespace rdr_scene::weapons {

inline constexpr int32_t kCWeaponSetDefault = 50, kCWeaponSetListed = 100;   // [C] the two set ids sub_825E0F60 looks up (0x825E1098..0x825E10A8)

// sub_825DDC70's set [C]: 1 for 51..59, 61, 62, 64, 68, 69.
inline constexpr bool CWeaponTakesListedSet(int32_t kind) { return (kind >= 51 && kind <= 59) || kind == 61 || kind == 62 || kind == 64 || kind == 68 || kind == 69; }
inline constexpr int32_t CWeaponSet(int32_t kind) { return CWeaponTakesListedSet(kind) ? kCWeaponSetListed : kCWeaponSetDefault; }

// The game's id of a recorded weapon id: a belt row carries kBeltIdBase + sub-key; the manager's id is 14.
inline constexpr int32_t CWeaponKindOfId(int32_t id) { return BeltSubOfId(id) >= 0 ? kWeaponIdBelt : id; }

// A stage row the CWeapon node drew (an own submit whose caller is the CWeaponPiece range: the only node stage_struct_nodes.h CasterBit marks).
inline constexpr bool IsCWeaponPieceRow(uint32_t slot_flags) { return IsStageCall(slot_flags) && IsStageOwn(slot_flags) && (slot_flags & kCallStageCaster) != 0; }
// Its CWeapon id: cage 175 + k [C sub_826FB030; L for CABU / CABA], cell 185 + k [C sub_826617B0], ladder case 23 [C sub_825DB328 +0xAC]; -1 = not known (CWeaponSet gives 50 for it too).
inline constexpr int32_t StagePieceKind(int32_t pach, uint32_t k) { return (pach >= 0 && pach <= 2) ? 175 + int32_t(k) : pach == 3 ? 185 + int32_t(k) : IsLadderPach(pach) ? 23 : -1; }

// Test only (tools\weapons\weapon_light_test.cpp --plant): 1 = the rule before WEAPONLIGHT (no pick: the caller's arena props set), 2 = every id set 50. The game never sets it.
inline int g_plant_weapon_light = 0;

// The set to light it with: the game's pick when the record holds that set (`has(id)`), else -1 (the caller keeps its older rule: a band-aid for the game's object defaults, rf_props.cpp CWeaponLights).
template <class HasFn>
inline int CWeaponLightKey(int32_t kind, HasFn&& has) {
  if (g_plant_weapon_light == 1) return -1;
  const int32_t s = g_plant_weapon_light == 2 ? kCWeaponSetDefault : CWeaponSet(kind);
  return has(s) ? int(s) : -1;
}

}  // namespace rdr_scene::weapons
