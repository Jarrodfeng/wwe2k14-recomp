// S111 PROPORDER (docs\research\s111\PROPORDER.md): every drawn place of the game's category-33 list, read at the latch: the characters' translucent nodes (their skeleton) AND the entrance
// props' CWeapon nodes (the model each one draws). Pure header (<cstdint>, <vector>, char_draw_list.h): the inputs latch (inputs_contrib.h Take) and tools\scene\proporder_unit.cpp call THIS code.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The list layout is CAT33's (char_draw_list.h). [C] sub_8272D710 (CWeaponEventSD::MakeWeapon) registers three nodes of each CWeapon in category 33 (+0x10F0 CWDT, +0x1188 WATR, +0x12B0 CWDS), each with
// the CWeapon at +48. The walker calls a node's slot 1 ([[node] + 4]): sub_8272C5F8 (CWDT, vtable 0x820296D0) draws the model at [cw + 1244], sub_8272C198 (WATR, 0x820296A8) [cw + 1248],
// sub_8272CCF0 (CWDS, 0x82029718) [cw + 1264] (+0x2E8 / +0x2B0 / +0x24C: slot 13 of that model), each under the node's own state (z write 0, blend 1, alpha test on; blend code 0 = alpha for CWDT /
// CWDS, 1 for WATR). MakeWeapon keeps the model of entry k at [cw + 1228 + 4 k] (+0x1320), so the three draw the prop's entries 4, 5 and 9. A place = the node's word index in the list (0 = first):
// one number space for both kinds, so a prop and a character compare directly.
#pragma once
#include <cstdint>
#include <vector>

#include "char_draw_list.h"

namespace scene_inputs::cat33_places {

inline constexpr uint32_t kSlotCwdt = 0x8272C5F8u, kSlotWatr = 0x8272C198u, kSlotCwds = 0x8272CCF0u;   // slot 1 of a CWeapon's three category-33 nodes
inline constexpr uint32_t kCwModelCwdt = 1244, kCwModelWatr = 1248, kCwModelCwds = 1264;              // the CWeapon word each one draws ([cw + 1228 + 4 k], k = 4 / 5 / 9)
inline constexpr uint32_t kVtSlot1 = 4;
// S111 PROPREKEY (docs\research\s111\PROPREKEY.md): the lists read are char_draw_list.h kDrawLists (33, 41, 43: the entrance re-key sub_827FBFE0 moves the characters' DRTR and the props' CWDT / WATR
// nodes to 43, the props' own nodes to 41 / 43), walked by WalkDrawLists; a place = list k's word i as k * kMaxNodes + i: one number space in the walk's order.
// S111 REKEYORDER (docs\research\s111\REKEYORDER.md): slot 1 of a CWeapon's own node (cw + 1200, vtable 0x820295F8, home category 26) is sub_8272D378: it draws [cw + 48] = [node - 1152] (+0x148), not
// [[node + 48] + 48], with blend 1 / code 0 / alpha test GEQUAL ref 1 (+0xB0..+0xEC) and the z write it inherits: 1 in 43 [C] (the reset sub_82C22E88 +0x50, no node before it leaves 0), the state it
// has in 26. The re-key (to 41, or 43 for ids 2064 / 2065 / 2555 / 2556 / 2621, +0xD8) moves its ORDER only: it gets a place with material_state (ours keeps the material's state, as in 26).
inline constexpr uint32_t kSlotOwn = 0x8272D378u;
inline constexpr uint32_t kOwnModelBack = 1152;   // sub_8272D378 +0x148: the own node draws the model word [node - 1152] = [cw + 48]
// TEST ONLY (tools\scene\proporder_unit.cpp --plant-own): the own node gives no place (the reader before S111 REKEYORDER). Written only by the unit before any read; the game never sets it.
inline bool g_plant_own_unplaced = false;
// S111 CATORDER (docs\research\s111\CATORDER.md): a character's OPAQUE node (char + 1200, slot 1 sub_825A3908; [node + 4] = its category, written by sub_82C30A10 / sub_82C30C10 +0x34 / +0x50; [node + 8] its
// enable) draws in its category's turn of the walk sub_82C2FA10 like every node. Its home, 13..21, is before every see-through category (22 on); the entrance re-key sub_827FBFE0 +0x3C moves it to 42 (when it is
// not in 33): after 30 (`s`), 33, 35, 36 and 41, before 43. Its place = the one just before the first walked list above its category (42: 2 * kMaxNodes - 1, after all of 41, before 43's first word). A category
// at or below 33 (drawn before every placed item: ours' opaque-first order) or one of the walked lists gives none.
inline constexpr uint32_t kCharSlotTable = 0x836F38D0u, kCharSlots = 9, kCharBodyNode = 1200, kNodeCat = 4;
// TEST ONLY (tools\scene\proporder_unit.cpp --plant-body): no body gives a place (every body drawn first, the order before S111 CATORDER). Written only by the unit before any read; the game never sets it.
inline bool g_plant_body_unplaced = false;

inline int32_t BodyPlaceOf(uint32_t cat) {
  namespace cdl = scene_inputs::char_draw_list;
  const uint32_t lists = cdl::g_plant_list33_only ? 1u : uint32_t(sizeof cdl::kDrawLists / sizeof cdl::kDrawLists[0]);
  if (cat <= cdl::kDrawLists[0]) return -1;
  for (uint32_t k = 0; k < lists; ++k) {
    if (cdl::kDrawLists[k] == cat) return -1;
    if (cdl::kDrawLists[k] > cat) return int32_t(k * cdl::kMaxNodes) - 1;
  }
  return int32_t(lists * cdl::kMaxNodes);
}

struct Place {
  uint32_t addr = 0;   // a character's skeleton, or the prop model a CWeapon node draws
  int32_t place = -1;  // list k's word index i as k * kMaxNodes + i (k = 0: category 33, 1: category 41, 2: category 43)
  bool prop = false;   // a CWeapon node (its model draws under the node's own blend state)
  bool additive = false;   // S111 PROPORDER review: the WATR node (sub_8272C198: blend code 1, SRC_ALPHA / ONE); CWDT / CWDS draw with code 0 (alpha)
  bool material_state = false;   // S111 REKEYORDER: a CWeapon's own node (kSlotOwn, prop = true): placed, its model keeps its material's state (the node's state is its category-26 one)
  bool body = false;   // S111 CATORDER: a character's opaque node (addr = its skeleton, place = BodyPlaceOf its category): where its body's opaque meshes draw
};

// The CWeapon word a node's slot 1 draws (0 = not a CWeapon node).
inline uint32_t PropModelWord(uint32_t slot1) {
  return slot1 == kSlotCwdt ? kCwModelCwdt : slot1 == kSlotWatr ? kCwModelWatr : slot1 == kSlotCwds ? kCwModelCwds : 0u;
}

// Every enabled node of categories 33, 41, 43 (char_draw_list.h WalkDrawLists), each in list order: a CWeapon node gives the model it draws (prop = true); a CWeapon's own node gives its model
// [node - 1152] (prop, material_state); any other node gives [[node + 48] + 48] (a character's skeleton: CAT33's rule), each address once (at its first node). false = a list could not be read (the
// walk's conditions): `out` is then empty and nothing is placed. An owner with fewer categories has those lists empty.
// S111 CATORDER: then each slot character whose opaque node is enabled in a category after 33 (BodyPlaceOf) gives its skeleton a body place (body = true); an unreadable word skips that slot only.
template <class Rd>
inline bool ReadPlaces(Rd&& rd, std::vector<Place>& out) {
  namespace cdl = scene_inputs::char_draw_list;
  out.clear();
  const bool ok = cdl::WalkDrawLists(rd, [&](uint32_t node, int32_t place) {
    uint32_t holder = 0, vt = 0, s1 = 0, addr = 0;
    const bool have_s1 = rd(node, vt) && cdl::Ptr(vt) && rd(vt + kVtSlot1, s1);
    const bool own = have_s1 && s1 == kSlotOwn;
    const uint32_t mw = have_s1 ? PropModelWord(s1) : 0u;
    if (own) {   // S111 REKEYORDER: the own node (re-keyed to 41 / 43) draws [node - 1152]; none there = it draws nothing (node is a pointer, >= 0x10000: no wrap)
      if (g_plant_own_unplaced || !rd(node - kOwnModelBack, addr) || !cdl::Ptr(addr)) return;
    } else if (!rd(node + cdl::kNodeChar, holder) || !cdl::Ptr(holder) || !rd(holder + (mw ? mw : cdl::kCharSkel), addr) || !cdl::Ptr(addr)) {
      return;   // a CWeapon with no model there draws nothing (the slot function's own test)
    }
    for (const Place& q : out)
      if (q.addr == addr) return;
    out.push_back(Place{addr, place, mw != 0 || own, mw == kCwModelWatr, own});
  });
  if (!ok) {
    out.clear();
    return false;
  }
  if (!g_plant_body_unplaced)
    for (uint32_t s = 0; s < kCharSlots; ++s) {   // S111 CATORDER: the slot table sub_827FBFE0 walks (+0x1C), the opaque node's category / enable words, the skeleton [char + 48]
      uint32_t ch = 0, cat = 0, en = 0, sk = 0;
      if (!rd(kCharSlotTable + 4 * s, ch) || !cdl::Ptr(ch) || !rd(ch + kCharBodyNode + kNodeCat, cat) || !rd(ch + kCharBodyNode + cdl::kNodeEnable, en) || en == 0) continue;
      const int32_t bp = BodyPlaceOf(cat);
      if (bp < 0 || !rd(ch + cdl::kCharSkel, sk) || !cdl::Ptr(sk)) continue;
      bool seen = false;
      for (const Place& q : out) seen = seen || (q.body && q.addr == sk);
      if (!seen) out.push_back(Place{sk, bp, false, false, false, true});
    }
  return true;
}

}  // namespace scene_inputs::cat33_places
