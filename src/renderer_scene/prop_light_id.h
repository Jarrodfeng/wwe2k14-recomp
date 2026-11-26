// S111 STAGELESS2 (docs\research\s111\STAGELESS2.md): the light set an EVENT PROP asks the game's light manager for, by id.
// Pure header (weapon_rows.h only). Writers: weapon_rows_core.h EventProps::ReadLightSets (the id, once a frame), weapon_rows_contrib.h (the section-59 event row's reserved[0]). Reader: rf_props.cpp PropLights.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C]: every node of a MakeWeapon prop that installs a light looks the id [cw + 1468] up in the light manager [0x83924778] (sub_827F6A40) and installs that set on a hit: the own node
// (sub_8272D378 0x8272D3F8, [node + 268] with node = cw + 1200), CWDT (sub_8272C5F8 0x8272C7B4), the vtable-0x820296A8 node (sub_8272C198 0x8272C31C) and CWDA (sub_8272D0F0 0x8272D264,
// [[node + 48] + 1468], node + 48 = the CWeapon: MakeWeapon 0x8272E94C). CWDS (sub_8272CCF0) installs none.
// Review [C]: CWDA installs through sub_827F6910 (the looked-up index, always); the other three through sub_826203E8(index, 1, 0), which installs the word [0x8362266C] instead while it is >= 0 and
// [0x836F53D0] is null or its +152 is 0 (written by the component ctor sub_82620070, reset to -1 by sub_82620250; its callers were never seen in a run) [L: never live in a match or seq 29].
// A missing id installs nothing (sub_827F6910). The base ctor zeroes the word (sub_83422628 0x83422820); Create a Title sets 10 on its belt (sub_83344760 0x8334487C, sub_8338E810 +D4), an entrance
// command 13 on prop 2038 (sub_8233C758). Every other event prop asks 0: it is lit by set 0 only where the record holds a set 0 (the game's lookup of 0 hits there too).
#pragma once
#include <cstdint>
#include <vector>

#include "weapon_rows.h"

namespace rdr_scene::weapons {

inline constexpr uint32_t kOffAskedLightSet = 1468;   // [C] the CWeapon's light-set id word (above)

// Test only (tools\weapons\event_props_test.cpp): 1 = the id is not carried (the rule before STAGELESS2: the event row's reserved[0] stays 0).
inline int g_plant_asked_light = 0;

// The section-59 event row's reserved[0]: the asked id + 1 (0 = not read / an older record / out of range).
inline uint32_t PackAskedLightSet(int32_t set) { return (set < 0 || set > 0xFFFF || g_plant_asked_light == 1) ? 0u : uint32_t(set) + 1u; }

// The id the event model `model` asks for, from the record's section-59 rows; -1 = none carried.
inline int32_t AskedLightSet(const std::vector<WeaponSlot>& slots, uint32_t model) {
  for (const WeaponSlot& s : slots)
    if ((s.flags & kSlotFlagEvent) != 0 && s.main == model && s.reserved[0] != 0) return int32_t(s.reserved[0] - 1u);
  return -1;
}

// The set to light it with: the asked id when the record holds that set (`has(id)`), else -1 (the game installs nothing on a miss; the drawer keeps its older rule).
template <class HasFn>
inline int PropLightKey(int32_t asked, HasFn&& has) {
  if (asked < 0) return -1;
  return has(asked) ? int(asked) : -1;
}

}  // namespace rdr_scene::weapons
