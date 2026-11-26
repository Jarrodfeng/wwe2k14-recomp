// The draw state of a gm set object (pac\gm.pac GMGA / GMGB banks, gmsk objects) that the gm manager draws in its category-36 node (S111 PODDOOR, docs\research\s111\PODDOOR.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: rf_props.cpp ApplyWeaponRows ors GmNodeObjectFlags into a gm row's object; tools\weapons\gmglass_unit.cpp checks it (planted twin --plant: the rule before, the material's state).
// [C] The gm manager [0x8376A9A4] walks the 12 controllers of [0x8376A9A0], each one's 512-word object table at +2120 (words 0..255 = keys 1000..1255, words 256..511 = keys 1500..1755, sub_827C8EB0):
//   category 23 sub_827C7888: blend 1 code 0, alpha test GEQUAL 1, z write 1 (+0x3C..+0x84); sub_827C8FF8 draws words 0..255 whose model MdlAlpha [model+252] >= 1.
//   category 36 sub_827C79A8: z write 0 (+0x58), blend 1 code 0, alpha test GEQUAL 1 (+0x70..+0xA0), z write 1 again at +0x13C; sub_827C90A8 draws words 0..255 whose MdlAlpha < 1 and EVERY object of
//   words 256..511 (keys 1500..1755) whatever its alpha.
// So a key-1500+ object (the Elimination Chamber's pod glass chem_glass02_ma, GMGA/0021 keys 1500 + 3 i) or a faded one is blended by code 0 with no depth write, after the wrestlers (13..21, 33):
// ours gives it the same object flags as the chamber's own category-36 node (stage_node_state.h): splat_state::NodeObjectFlags + kObjNodeLate. A category-23 draw keeps its material's state (z write on).
// Not told apart, and not needed [C, PODDOOR review]: [obj+1192] (sub_827C9BC8 +0x12C: blend code 1) and [obj+1232] (an alpha-ref-255 split) are set only by GMEV scene-object types 7 / 23, and no
// shipped gm.pac row of those types sets them (PODDOOR.md claims 0x827D33A0 / 0x827D2330).
#pragma once
#include <cstdint>

#include "scene_record.h"
#include "splat_state.h"

namespace rdr_scene::gm_node {

inline constexpr uint32_t kGmTailKeyLo = 1500, kGmTailKeyHi = 1755;   // words 256..511 of a controller's table (sub_827C8EB0; weapon_rows_core.h PullGm's key ranges)

// Does the gm manager draw this object in its category-36 node? (sub_827C90A8: a tail key always; a head key when its MdlAlpha is below 1, the compare of +0x44, so a NaN alpha stays in category 23.)
inline constexpr bool DrawnInCategory36(uint32_t key, float mdl_alpha) { return (key >= kGmTailKeyLo && key <= kGmTailKeyHi) || mdl_alpha < 1.f; }

// The object flags of one gm row: the category-36 node's (no depth write, alpha blend code 0, a category after 33) or 0 (category 23: the material's state). plant_old = the rule before: tests only.
inline uint32_t GmNodeObjectFlags(uint32_t key, float mdl_alpha, bool plant_old = false) {
  if (plant_old || !DrawnInCategory36(key, mdl_alpha)) return 0u;
  return splat_state::NodeObjectFlags(false) | uint32_t(kObjNodeLate);
}

}  // namespace rdr_scene::gm_node
