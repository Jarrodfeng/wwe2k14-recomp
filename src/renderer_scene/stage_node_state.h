// The draw state of a stage structure piece that the structure's own node draws under the NODE's state, whatever its material (S111 PODGLASS, docs\research\s111\PODGLASS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: rf_props.cpp ApplyWeaponRows ors StageNodeObjectFlags into a stage row's object; tools\weapons\podglass_unit.cpp checks it (planted twin --plant: the rule before, the material's state).
// [C] the Elimination Chamber's components 'CDTB' sub_826F8318 (render category 35) and 'CDTA' sub_826F8398 (category 36) call sub_826F8F88(C, 0 / 1) (vtables 0x820280C0 / 0x820280C8 slot 1 = sub_826F9770 / sub_826F9780).
// sub_826F8F88 +0x10C..+0x154 sets blend enable 1 (sub_82BB75A0), alpha test 1 (sub_82BB75E8), alpha ref 1 (sub_82BB7928), alpha func GEQUAL (sub_82BB7678 5), blend code 0 = SRC_ALPHA / INV_SRC_ALPHA
// (sub_82BB7630), z write 0 (sub_82BB7AD8), and restores the saved z write at +0x7D4. Category 35 draws k 1 / 3 (the chain fences); category 36 draws k 4 / 6 / 7 (pod frames, doors, front wall) at the camera
// fade, k 5 (the pod glass) at four quarter turns at MdlAlpha 0.9 or the fade, and k 8 (the spotlight) with blend code 1 = SRC_ALPHA / ONE (+0x710..+0x7C8). These categories come after the wrestlers' (13 + slot, 33),
// so the glass is blended over the bodies with no depth write. Ours draws every row of this node with the object flags of the CWeapon category-33 nodes (splat_state.h NodeObjectFlags): no depth write, blended by
// the node's code; the row's MdlAlpha stays its tint (rf_props.cpp). The category-3 node sub_826F8958 sets no state: its rows keep their materials' (class kStageLrOther).
// The record tags the node by the caller of the slot-13 dispatcher (weapon_rows.h OwnCallerClass -> kStageLrOwnNode).
// S111 PODGLASS review: with no depth write the ORDER is what keeps a fence wire / pane over what is behind it. The game's walk draws categories 35 / 36 after every category-33 draw (the wrestlers' translucent
// pieces: hair, gear), so the rows also carry kObjNodeLate: in Game order their alpha draws take the rank after the character rank (see_through_order.h SeeRank), in their record (= the node's draw) order.
#pragma once
#include <cstdint>

#include "scene_record.h"
#include "splat_state.h"
#include "weapon_rows.h"

namespace rdr_scene::stage_node {

// The node's blend code for piece k of PACH `pach`: 1 (SRC_ALPHA / ONE) for the chamber's spotlight k 8 (sub_826F8F88 +0x710 sub_82BB7630(1) .. +0x7C8 back to 0), 0 for every other piece it draws.
inline constexpr bool NodeAdditive(int32_t pach, uint32_t k) { return pach == 6 && k == 8; }

// The object flags of one stage row: the node's (no depth write + alpha / additive blend) for an own call made by such a node, 0 otherwise. plant_old = the rule before (the material's state): tests only.
inline uint32_t StageNodeObjectFlags(uint32_t slot_flags, int32_t pach, uint32_t k, bool plant_old = false) {
  if (plant_old || !weapons::IsStageCall(slot_flags) || !weapons::IsStageOwn(slot_flags) || weapons::StageLrClass(slot_flags) != weapons::kStageLrOwnNode) return 0u;
  return splat_state::NodeObjectFlags(NodeAdditive(pach, k)) | uint32_t(kObjNodeLate);
}

// S111 CHAMBERFADE (docs\research\s111\CHAMBERFADE.md): the tint of a row drawn under such a node. The node blends every piece by its MdlAlpha with an alpha test GEQUAL 1/255 (sub_826F8F88 +0x10C..+0x154),
// and it writes the fade as MdlAlpha: the front wall k 7 gets the raw fade, 0 when the camera is within 2000 of the fade point (+0xDC, +0x52C; no 0.2 floor), so 0 means "drawn as nothing", never "not set".
// A row with the node's flags carries 0 <= MdlAlpha < 1 as its tint (blended to nothing at 0, z write off); any other row keeps `tint` (rf_props' rule). plant_old = the rule before (0 -> tint 1): tests only.
inline float StageNodeTint(uint32_t object_flags, float mdl_alpha, float tint, bool plant_old = false) {
  if (plant_old || splat_state::NodeBlendOf(object_flags) == 0) return tint;
  return (mdl_alpha >= 0.f && mdl_alpha < 1.f) ? mdl_alpha : tint;
}

}  // namespace rdr_scene::stage_node
