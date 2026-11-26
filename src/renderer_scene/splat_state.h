// The floor blood splats' draw state and the front end's per-object depth-write rule (Session 108, lane BLD2; docs\research\s108\BLD_plan.md sections 1 and 10). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: rf_props.cpp sets the splat's object flags with SplatObjectFlags, front_end.cpp decides every mesh's depth write with DepthWriteOf; tools\weapons\gbr_test.cpp checks both (planted twin splat_depth_write).
// [C] the game's splat node sub_82698770 (0x82698898..0x826988E0, the recorder wrappers of docs\research\s103\T2_render_states.md): z enable 1, z WRITE 0 (sub_82BB7AD8 = slot 18), blend 1 (code 0), alpha test 1, GEQUAL, ref 1.
#pragma once
#include <cstdint>

#include "scene_record.h"

namespace rdr_scene::splat_state {

// A splat: blended by its record's alpha over an opaque material, and no depth write (the game's). plant_old = the twin (the old flags: force-blend alone).
inline uint32_t SplatObjectFlags(bool plant_old = false) {
  return kObjForceBlend | (plant_old ? 0u : uint32_t(kObjNoDepthWrite));
}

// The front end's depth write of one mesh draw: an opaque (unsorted) draw writes depth, a sorted one does not unless it is a prop cut-out drawn blended in Game order (S103 F); an object the game draws with z write off never does.
inline bool DepthWriteOf(uint32_t object_flags, bool sorted, bool game_order_cutout) {
  return !(object_flags & kObjNoDepthWrite) && (!sorted || game_order_cutout);
}

// S111 PROPORDER review (docs\research\s111\PROPORDER.md A.3): an entrance prop's model a CWeapon category-33 node draws takes the NODE's state, whatever its material. [C] CWDT sub_8272C5F8 +0x230..+0x278 and CWDS
// sub_8272CCF0 +0x1B4..+0x1FC: z enable 1, z WRITE 0 (sub_82BB7AD8), blend 1, alpha test 1 GEQUAL ref 1, blend code 0 (SRC_ALPHA / INV_SRC_ALPHA); WATR sub_8272C198 +0x1F8..+0x240: the same with blend code 1 (SRC_ALPHA / ONE).
// The node draws the model through its vtable slot 13 (+0x2E8 / +0x2B0 / +0x24C): sub_82C13C88, in the colour pass (both per-thread pass flags 0) sub_82C04DF0 (or, for a cached model, sub_82BF8118), which set
// cull by the mesh record's bit (0x82C0A8F0, the family_cull.h rule ours already applies) and the winding, and never the blend enable, the blend code, z write or the alpha test: what reaches the draw is the node's
// own state, blended by its code with no depth write. (The lambert pass sub_82BF5C58 is vtable slot 43, not reached from slot 13. The node's alpha test, GEQUAL ref 1, is not carried: with z write off it only drops
// texels the blend already shows as nothing.)
inline uint32_t NodeObjectFlags(bool additive) {
  return uint32_t(kObjNoDepthWrite) | (additive ? uint32_t(kObjNodeAdditive) : uint32_t(kObjNodeAlpha));
}
// S111 REKEYORDER review: the object flags a placed object takes from its place (draw_place_rows.h Carry's DrawPlace, the one rule rf_props.cpp and tools\scene\proporder_unit.cpp call): a CWeapon
// category-33 node's place (block) gives the node's state; a CWeapon own node's place (material_state) only orders it, the material's state kept; a character's place (not block) gives none.
template <class Place>
inline uint32_t PlacedObjectFlags(const Place& p) {
  return p.block && !p.material_state ? NodeObjectFlags(p.additive) : 0u;
}
// S111 PODGLASS review: the object's node is of a category after 33 (scene_record.h kObjNodeLate): FrontEnd::Record's see-through key (SeeKey::late_node).
inline bool LateNodeOf(uint32_t object_flags) { return (object_flags & kObjNodeLate) != 0; }
// The front end's blend for a mesh of an object a node draws: 1 alpha, 2 additive; 0 = no node (the material decides, as before).
inline uint8_t NodeBlendOf(uint32_t object_flags) {
  return (object_flags & kObjNodeAdditive) ? uint8_t(2) : (object_flags & kObjNodeAlpha) ? uint8_t(1) : uint8_t(0);
}

// S111 OWNNODEZ (docs\research\s111\OWNNODEZ.md): the CWeapon's OWN node sub_8272D378 draws its model [node-1152] (= [cw+48]) with z enable 1 (+0xB0), blend 1 (+0xBC), alpha test 1 GEQUAL ref 1 (+0xC8..+0xE0)
// and blend code 0 (+0xEC), and no z write of its own: the category reset's 1 (sub_82C22E88 +0x50) [C]. Every yProp output alpha carries MdlAlpha (fx_prop.glsl:363 / :604-606 / :781-783), so the game fades the whole
// model by it. Ours blends + tests the binds whose own alpha its bind rules see (front_end.cpp GameTestsAlpha + the B2 tested blend, depth write kept); MdlAlpha is an object value no bind sees.
// S111 OWNNODEZ review: the CWDA node sub_8272D0F0 (entry 6, category 26) sets the same state with alpha ref 128 (+0x198..+0x1D4: z enable 1, blend 1, test 1 GEQUAL ref 128, code 0; no z write) [C]: kObjNodeRef128.
// So for such an object (the game's code, every mesh): MdlAlpha below the node's reference = nothing (an output alpha is never above MdlAlpha: (diffuse.a or 1) x vertex.a x MatAmbCol.w x MatDifCol.w x MdlAlpha,
// the w terms 1 on the evtobj meshes [L for the effects not asked]); otherwise blended by code 0, in the material's list with its depth write (the node's z write 1). Where a mesh's output alpha is 1 the
// blend changes no pixel; where it is not (a part GameTestsAlpha / YbgTestsAlpha leave untested: a vertex alpha of a yProp_PSM / UVScroll / yBG_Default part, a variant without spirv_alpha) it is the game's look.
// Not carried: CWDA's per-texel 128/255 test of a tested part (ours tests it at the lit class's 1/255, front_end.cpp B2).
inline uint32_t OwnNodeObjectFlags(bool cwda = false) { return uint32_t(kObjNodeOwn) | (cwda ? uint32_t(kObjNodeRef128) : 0u); }
// TEST ONLY (tools\scene\ownnode_unit.cpp --plant): the rule before OWNNODEZ (every mesh keeps its material's draw at any MdlAlpha). Written only by the unit; the game never sets it.
inline bool g_plant_own_node_material = false;
enum class OwnDraw : uint8_t { AsMaterial, Blend, Nothing };
inline OwnDraw OwnNodeDraw(uint32_t object_flags, float item_alpha) {
  if (g_plant_own_node_material || !(object_flags & kObjNodeOwn)) return OwnDraw::AsMaterial;
  const float ref = (object_flags & kObjNodeRef128) ? 128.f / 255.f : 1.f / 255.f;
  return item_alpha >= ref ? OwnDraw::Blend : OwnDraw::Nothing;
}
// What FrontEnd::Record does with one mesh item (the one place it is decided; tools\scene\ownnode_unit.cpp calls this same function): false = the item is not drawn; otherwise `blend` (Item::blend,
// 0 = the material's unblended draw) becomes the node's code 0 when the material does not blend already. The list and the depth write stay the material's.
inline bool ApplyOwnNode(uint32_t object_flags, float item_alpha, bool has_material, uint8_t& blend) {
  const OwnDraw od = OwnNodeDraw(object_flags, item_alpha);
  if (od == OwnDraw::Nothing) return false;
  if (od == OwnDraw::Blend && has_material && !blend) blend = 1;
  return true;
}

}  // namespace rdr_scene::splat_state
