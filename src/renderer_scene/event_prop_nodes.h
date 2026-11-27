// S111 WATRSHADOW (docs\research\s111\WATRSHADOW.md): which of an entrance prop's draw nodes draws its PACH entry k, and so which of the game's shadow maps that model is cast into.
// Pure header (weapon_rows.h only): weapon_plan.h (CastsMapB / CastsPropsMap / Plan), rf_props.cpp (the map-B scene-word bit) and tools\weapons\evt_node_cast_test.cpp call THIS code.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// [C] CWeaponEventSD::MakeWeapon (sub_8272D710) builds ONE CWeapon per prop: the first made entry is the CWeapon's own model [cw+48], drawn by the CWeapon's own node (cw+1200, vtable 0x820295F8,
// slot 1 sub_8272D378: category 26, re-keyed to 41 / 43 by sub_827FBFE0 at an entrance); every later entry k sits at [cw+1228+4k] (+0x1320), and four nodes draw four of those words:
//   CWDT (category 33, slot 1 sub_8272C5F8) [cw+1244] = entry 4: skips ids 2065 / 2114 / 2511 / 2555 / 2621 / 2642 under the shadow-render flag (+0x58..+0x98), as the own node does (+0x3C..+0x78);
//   WATR (category 33, sub_8272C198)        [cw+1248] = entry 5: draws only when the flag is 0 (+0x60): in no shadow map;
//   CWDA (category 26, sub_8272D0F0)        [cw+1252] = entry 6: no flag test, no id test;
//   CWDS (category 33, sub_8272CCF0)        [cw+1264] = entry 9: no flag test, no id test.
// No CWeapon reader of [cw+1228+4k] for k = 1, 2, 3, 7, 8 is in the field index [L]: such an entry keeps the own node's rule (no row of one is expected: S111 GEARSLOTS,
// no disc or DLC evtobj PACH holds an entry 1, 2, 3, 7 or 8 [C asset index census]).
// The passes [C]: the props map (sub_827E7F00) draws categories 24 / 26 / 41 only; map B (sub_827E7338, mode 0) draws 26 / 41, then 33 + 43 unless the scene word [0x836F3940] is 1.
#pragma once
#include <cstddef>
#include <cstdint>

#include "weapon_rows.h"

namespace rdr_scene::weapons::evt_nodes {

enum class Node : uint8_t { Main, Cwdt, Watr, Cwda, Cwds };

// Planted twin (tools only; the game never does): every entry is taken for the own node's model (the rule before S111 WATRSHADOW).
inline bool g_plant_entry_blind = false;

// The node that draws entry k of a prop whose first made entry is main_k.
inline Node NodeOf(uint32_t k, uint32_t main_k) {
  if (g_plant_entry_blind || k == main_k) return Node::Main;
  return k == 4 ? Node::Cwdt : k == 5 ? Node::Watr : k == 6 ? Node::Cwda : k == 9 ? Node::Cwds : Node::Main;
}

// Does the node draw while the shadow-render flag is set (any shadow map)? WATR does not.
inline constexpr bool DrawsUnderFlag(Node n) { return n != Node::Watr; }
// Does the node apply the six-id skip under the flag? The own node and CWDT do; CWDA and CWDS have no id test.
inline constexpr bool HonoursSkipIds(Node n) { return n == Node::Main || n == Node::Cwdt; }
// Is the node in a category the props pass draws (26 / 41)? The own node unless it was re-keyed to 43 (piece43_id), and CWDA; never CWDT / WATR / CWDS (33, or 43 after the re-key).
inline constexpr bool InPropsPass(Node n, bool piece43_id) { return n == Node::Main ? !piece43_id : n == Node::Cwda; }
// Does map B draw it only when the scene word is not 1 (categories 33 / 43)? CWDT and CWDS, and the own node when re-keyed to 43; not CWDA, not an own node in 26 / 41.
inline constexpr bool SceneWordGated(Node n, bool piece43_id) { return n == Node::Main ? piece43_id : (n == Node::Cwdt || n == Node::Cwds); }
// Is the node registered in category 33 (MakeWeapon +0x10F0 / +0x1188 / +0x12B0: CWDT / WATR / CWDS)? The own node (cw+1200: 26, or 41 / 43 after sub_827FBFE0) and CWDA (26, +0x1220) are not. rf_props.cpp's PROPORDER place census asks this.
inline constexpr bool InCategory33(Node n) { return n == Node::Cwdt || n == Node::Watr || n == Node::Cwds; }

// The first made entry of prop `id`: the lowest k among the record's section-59 event rows of that id (weapon_rows.h kSlotFlagEvent: slot = k); 0 when there is none (an older record, a unit).
inline uint32_t MainEntry(const WeaponSlot* slots, size_t n, int32_t id) {
  uint32_t best = kMaxEventModels;
  for (size_t i = 0; slots && i < n; ++i)
    if ((slots[i].flags & kSlotFlagEvent) && slots[i].id == id && (slots[i].slot & kCallEntryMask) < best) best = slots[i].slot & kCallEntryMask;
  return best < kMaxEventModels ? best : 0u;
}

}  // namespace rdr_scene::weapons::evt_nodes
