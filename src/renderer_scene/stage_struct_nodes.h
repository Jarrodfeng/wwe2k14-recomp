// S111 CHAMBERSHADOW (docs\research\s111\CHAMBERSHADOW.md): which draw node submitted a stage structure piece (the caller of the slot-13 dispatcher sub_82C13C88), and so whether the game casts that draw
// into its shadow maps. Pure header (cstdint only): weapon_rows.h includes it; the recorder (weapon_rows_core.h, the bit at the call), weapon_plan.h (CastsMapB / CastsPropsMap) and
// tools\weapons\stage_node_cast_test.cpp call THIS code.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// [C] The shadow passes walk render categories: map B sub_827E7338 24 (gated) / 13..21 + 42 (sub_827E6F40) / 26 / 41 / 33 / 43; the props map sub_827E7F00 24 / 26 / 41; map A sub_827E8448 13..21 + 42;
//     sub_82813228 the circle blobs only. They are the only setters of the shadow flag 0x83929A48 + 4 x slot (sub_82BA7778: 8 call sites, all in these four).
// [C] The structures' own nodes (ctor +offset = the category registration): cage sub_826FB030 +0x28 category 26 (slot 1 sub_826FA0B8: draws only with the flag 0, +0x20 / +0x28) and +0x198 category 36
//     (the slot-32 family); cell sub_826617B0 +0x28 category 27 (sub_8265FEB0, flag 0 only) and +0x2A0 category 36 (sub_826617A8 -> sub_82660628, slot 32); ladder LDBL sub_825DC218 +0x30 category 23
//     (sub_825DB710 the rope, flag 0 only) and +0x130 category 35; Inferno sub_8266A568 +0x34 category 23 (sub_82669A68, no flag test); chamber sub_826F8418 category 3 (sub_826F8958, flag 0 only,
//     +0x20 / +0x28) and CDTB 35 / CDTA 36 (sub_826F8F88, flag 0 only, +0x30 / +0x38). No pass walks 3 / 23 / 27 / 35 / 36, and the one walked node (the cage's 26) returns under the flag: no structure
//     node's draw is cast.
// [C] The pieces built as CWeapons (cage k 0..8 = id 175 + k, cell id 185 + k: sub_824D6C68 -> sub_825E2DE0; the ladder's belt / case) are drawn by the CWeapon node (vtable 0x820200D8, slot 1 sub_825DFA78,
//     its slot-13 call at +0x4B0), which under the flag asks the object's slot 16 (sub_825DDE38: 0 only for ids 6 / 7 / 29 / 31 / 32 / 34 / 35 / 37 / 104..107) and then draws: these pieces cast.
//     Their category is 26 [C, S111 CHAMBERSHADOW review]: sub_825E0F60 -> sub_83422628 (+0x10 r5 = desc +20) -> sub_823E6D18 +0x68 registers the node at +1200 in category r5, and sub_825E2DE0's
//     model path (r3 = the model, 0x825E3638) builds the desc with sub_834223A0 (+20 = 26) and never changes +20 (the manager path's 26 + 6 x (id 59 / 68 / 71 / 96) is not reached).
// [C] The other own submitters of a holder model (every slot-13 call site whose r3 is a holder model: the callers of the getter sub_825766A8 and of [0x837631C4], S111 CHAMBERSHADOW review):
//     the ladder lights sub_825DB7B8 (+0x11C / +0x170; the category-35 node via sub_825DC210, flag 0 only, +0x28 / +0x30) and the cell's helpers sub_8265F158 (+0x58) / sub_8265FAE0 (+0x198),
//     reached only from the cell's category-27 / 36 nodes, which return under the flag. A caller outside the table is not cast (none is known).
#pragma once
#include <cstdint>

namespace rdr_scene::weapons {

// Bit 31 of a STAGE row's slot_flags (kCallStageStruct set): the own submit came from a node the game's shadow passes draw (stg_nodes::CastsIntoShadowMaps). Bits 30-31 are the specular code on the
// other rows (prop_spec.h kCallSpecShift); a stage row never carries one (weapon_rows_core.h packs it only when !is_stage, rf_props.cpp reads it only when !it.stage).
inline constexpr uint32_t kCallStageCaster = 0x80000000u;

namespace stg_nodes {

enum class Node : uint8_t { Unknown, CWeaponPiece, CageNode, CellNode, LadderRope, LadderLights, InfernoNode, ChamberFloorNode, ChamberSeeNode };

struct NodeRange {
  uint32_t lo, hi;   // [lo, hi) = the function that makes the slot-13 call (the node's slot-1 draw, or a helper only that node calls; q decomp sizes)
  Node node;
  int32_t category;  // its render category (the ctor's registration)
};
inline constexpr NodeRange kNodes[] = {
    {0x825DFA78u, 0x825DFF58u, Node::CWeaponPiece, 26},     // sub_825DFA78, 1248 bytes (the desc's 26; an entrance re-key to 41 is walked too)
    {0x826FA0B8u, 0x826FA7C4u, Node::CageNode, 26},         // sub_826FA0B8, 1804
    {0x8265FEB0u, 0x82660628u, Node::CellNode, 27},         // sub_8265FEB0, 1912
    {0x8265F158u, 0x8265F1D0u, Node::CellNode, 27},         // sub_8265F158, 120 (called by the cell nodes' sub_8265FAE0 / sub_8265FCA0 / sub_8265FDE8)
    {0x8265FAE0u, 0x8265FCA0u, Node::CellNode, 27},         // sub_8265FAE0, 448 (called by sub_8265FEB0 +0x5A8 and sub_82660628 +0x868)
    {0x825DB710u, 0x825DB7B8u, Node::LadderRope, 23},       // sub_825DB710, 168
    {0x825DB7B8u, 0x825DB968u, Node::LadderLights, 35},     // sub_825DB7B8, 432 (ladder holder models 2 / 3)
    {0x82669A68u, 0x82669B64u, Node::InfernoNode, 23},      // sub_82669A68, 252
    {0x826F8958u, 0x826F8EACu, Node::ChamberFloorNode, 3},  // sub_826F8958, 1364
    {0x826F8F88u, 0x826F9770u, Node::ChamberSeeNode, 35},   // sub_826F8F88, 2024 (categories 35 and 36: neither walked)
};

// Planted twin (tools only; the game never does): the node is ignored, every own submit but the cage node's casts (the rule before S111 CHAMBERSHADOW).
inline bool g_plant_node_blind = false;

inline constexpr Node NodeOfCaller(uint32_t caller) {
  for (const NodeRange& r : kNodes)
    if (caller >= r.lo && caller < r.hi) return r.node;
  return Node::Unknown;
}
inline constexpr int32_t CategoryOf(Node n) {
  for (const NodeRange& r : kNodes)
    if (r.node == n) return r.category;
  return -1;
}
// Does a shadow pass walk the category? Map B 24 / 26 / 41 / 33 / 43, the props map 24 / 26 / 41 (map A and the blobs draw wrestlers only).
inline constexpr bool InShadowPass(int32_t category) { return category == 24 || category == 26 || category == 41 || category == 33 || category == 43; }
// Does the node draw while the shadow flag is set? The CWeapon node (its slot-16 test passes every stage piece id) and the Inferno's (no flag test); the cage / cell / rope / ladder-light / chamber nodes return.
inline constexpr bool DrawsUnderFlag(Node n) { return n == Node::CWeaponPiece || n == Node::InfernoNode; }
// The rule: a node's draws are cast when a pass walks its category and it draws under the flag. Only the CWeapon pieces; an unknown caller is not cast (the table names every known submitter).
inline constexpr bool CastsIntoShadowMaps(Node n) { return n != Node::Unknown && InShadowPass(CategoryOf(n)) && DrawsUnderFlag(n); }
// The recorder's bit for a stage call (weapon_rows_core.h): set on an own submit from a casting node.
inline constexpr uint32_t CasterBit(bool own_submit, uint32_t own_caller) { return own_submit && CastsIntoShadowMaps(NodeOfCaller(own_caller)) ? kCallStageCaster : 0u; }

}  // namespace stg_nodes
}  // namespace rdr_scene::weapons
