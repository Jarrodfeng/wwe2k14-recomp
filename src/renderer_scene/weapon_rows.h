// Weapons and props in a match: the scene-record contract shared by lanes PW-1 (slot reader), PW-2 (per-call rows) and PW-3 (drawer).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; no game, SDK or asset includes.
// Session 103 (2026-11-17). Evidence: docs\research\s103\PW_weapons_props.md (probe), docs\research\s103\PW_adversary.md (adversary: every offset re-derived).
// Marks: [C] read in a listing, [L] a reading, [U] unknown.
//
// The game's weapon manager (a singleton made at boot by sub_822A7400) [C]:
//   mgr = u32[0x837635B4]; slot count n = u32[mgr+580] (<= 48); slot i: id = s32[mgr+388+4i] (after the 29/32/35 remap of sub_825DDAB0; -1 = empty),
//   main model = u32[mgr+848+4i] (a 912-byte skinned model, vtable 0x8206AF90), secondary models = u32[mgr+1048+16i+4k], k < 2.
//   Slots are written at match load only (sub_825910D8 -> SetWeaponData sub_8258FF90 -> MakeSkinModel sub_8258E270); a reload (sub_82592608 -> sub_82590ED8) frees every slot
//   model and resets the table, and freed addresses can come back for another id: the table is pulled EVERY frame, never cached across frames.
// The draw [C]: CWeapon draw sub_825DFA78 memcpys the weapon's own matrices into the SHARED model ([model+200] / [model+192]), then vcall +52 -> sub_82C13C88 ->
//   sub_82C04DF0(r3 = model, r5 = [model+200]); the model-entry hook at 0x82C04E1C sees it. ONE model object is shared by every weapon of an id (two chairs, two step sets
//   draw with the same r3 AND the same r5 pointer): a world is copied from r5 AT EACH CALL, one row per call, never deduped on (object, r5) and never read from [model+200]
//   at emit. When [w+1464] != 0 the weapon draws a SECONDARY model for that submit: the slot table maps main AND secondary objects to the id.
//   [U]: when [model+560] != 0, sub_82C13C88 takes a cached command-buffer path that skips sub_82C04DF0; the census counts "named but never drawn".
#pragma once
#include <cstdint>

#include "stage_struct_nodes.h"   // S111 CHAMBERSHADOW: kCallStageCaster + the stage draw node table (weapon_rows_core.h tags, weapon_plan.h CastsMapB reads)

namespace rdr_scene::weapons {

inline constexpr uint32_t kMgrGlobal = 0x837635B4;   // [C] PW_adversary.md 1
inline constexpr uint32_t kOffSlotCount = 580;       // [C]
inline constexpr uint32_t kOffSlotId = 388;          // + 4i [C]
inline constexpr uint32_t kOffSlotMain = 848;        // + 4i [C]
inline constexpr uint32_t kOffSlotSecondary = 1048;  // + 16i + 4k [C]
inline constexpr uint32_t kMaxSlots = 48;            // [C] the game's cap (a 360 habit: our rows are not limited by it)
inline constexpr uint32_t kMaxSecondary = 2;         // [C] getter sub_8258CB58 allows k in 0..1; sub_8258FF90 loops k < 2. Each 16-byte slot block is
                                                      // {data0, data1, obj0, obj1}: the MODEL objects are at 1048+16i+4k, k < 2 (the PW-1/2 review,
                                                      // S103 21:52: the first contract said 4 and read slot i+1's data objects)
inline constexpr uint32_t kSkinnedModelVtable = 0x8206AF90;   // [C] check u32[main] == this before trusting a slot

// Ring steps are ids 29 / 32 / 35 [C remap, L name]; S103 LDR: drawn from the weapon rows (weapon_plan.h Plants::draw_steps, default true); the arena loader's fixed copy only for a record with no weapon table (ArenaFixedPropsDrawn).
inline constexpr bool IsRingStepsId(int32_t id) { return id == 29 || id == 32 || id == 35; }

// Section ids (scene_record.h's free range; 59-61 belong to this contract).
inline constexpr uint32_t kScWeaponSlots = 59;   // WeaponSlot[]: one row per live slot per frame (PW-1, pulled at emit)
inline constexpr uint32_t kScWeaponCalls = 60;   // WeaponCall[]: one row per sub_82C04DF0 call whose r3 is a weapon model (PW-2, copied at the call)
inline constexpr uint32_t kScWeaponNodes = 61;   // float[12] per node: the node rows the WeaponCall rows index (VS c76..c78 of node n, ringrows::RowsFromMatrix layout)

struct WeaponSlot {          // 32 bytes
  int32_t id = -1;           // the game's weapon id (after the remap); the m2.pac p0/1000/<id> key
  uint32_t slot = 0;         // i
  uint32_t main = 0;         // guest address of the main model object
  uint32_t secondary[kMaxSecondary] = {};   // guest addresses, 0 = none
  uint32_t reserved[2] = {};                // keeps the row at 32 bytes; 0, except reserved[0] of an EVENT row (kSlotFlagEvent): the light set its CWeapon asks for + 1 (S111 STAGELESS2, prop_light_id.h)
  uint32_t flags = 0;        // bit 0: main's vtable checked == kSkinnedModelVtable
};
static_assert(sizeof(WeaponSlot) == 32, "WeaponSlot layout");

// One row per call. A record carries the calls of the NEWEST closed frame only (the contributor drops older ones: PW review fix 7), yet every row says which frame it belongs to and which id
// its object was at THAT call (the table is read in the same frame as the call: no join across a reload). PW-1/2 review, S103.
struct WeaponCall {          // 32 bytes
  uint32_t object = 0;       // r3 at the call
  uint32_t instance = 0;     // 0, 1, 2 ... : the n-th call of this object in this frame (two chairs = instance 0 and 1)
  uint32_t frame = 0;        // guest frame of the call (low 32 bits); the drawer uses only the newest frame in a record
  int32_t id = -1;           // the weapon id of `object` in that frame's slot table (-1 = not found)
  uint32_t node_first = 0;   // index into kScWeaponNodes (in nodes)
  uint32_t node_count = 0;   // nodes copied from r5 at the call (node 1..N, ring_rows rule)
  float alpha = 1.0f;        // [model+252] at the call (slot-13 submits pass it as f1) [L]
  uint32_t slot_flags = 0;   // bits 0-7 slot i; bit 8 = a secondary model (kCallSecondary); bit 9 = the slot's main model passed the vtable check (kCallVtableOk); bits 10-11 = k, the secondary's index (0 / 1; meaningful with bit 8); bit 12 = an event prop (kCallEventProp, S103 BLF), bits 16-19 = its entry k; bits 30-31 = the model's run-time specular set (S111 PROPSPEC, prop_spec.h kCallSpecShift); other bits reserved, 0
};
static_assert(sizeof(WeaponCall) == 32, "WeaponCall layout");
inline constexpr uint32_t kCallSecondary = 0x100;
inline constexpr uint32_t kCallVtableOk = 0x200;
inline constexpr uint32_t kCallKShift = 10, kCallKMask = 0x3;
// S103 BLF (docs\research\s103\BELT.md Adversary): an EVENT PROP call (a model made by CWeaponEventSD::MakeWeapon sub_8272D710: the entrance's title belt, microphone, cap, held item, stage prop). Same row, same 32 bytes: `id` is the
// PROP id (the entrance's already-rewritten id, e.g. Cena's belt 2004), bit 12 = kCallEventProp, bits 16-19 = k, the entry (0..9) of the prop's evtobj.pac PACH this model was built from. The PACH itself is resolved by the loader
// from the id (assets::PropPach), never recorded. A reader that does not know bit 12 would take `id` for a weapon-manager id and load nothing (m2.pac has no such id): no layout bump is needed.
inline constexpr uint32_t kCallEventProp = 0x1000;
inline constexpr uint32_t kCallEntryShift = 16, kCallEntryMask = 0xFF;   // S103 CGF: widened from 4 bits (a stage structure's key k reaches 16: the cell has 17 keys); bits 24-31 hold the stage LR class below
inline constexpr bool IsEventCall(uint32_t slot_flags) { return (slot_flags & kCallEventProp) != 0; }
inline constexpr uint32_t EventEntry(uint32_t slot_flags) { return (slot_flags >> kCallEntryShift) & kCallEntryMask; }
// S103 CGF (docs\research\s103\CAGE.md, its Adversary section; docs\formats\bgetc_stg_structures.md): a STAGE STRUCTURE call (steel cage CAGE / CABU / CABA, Hell in a Cell, Elimination Chamber, Inferno): a model of the ONE holder
// H = [0x837631C4] (slots [H+76+4k], filled from pac\bg\bgEtc.pac). Same row, same 32 bytes (no layout change: only bits that were reserved 0): `id` = the bgEtc PACH (0 CAGE / 1 CABU / 2 CABA / 3 CELL / 6 CHAI / 7 INFE),
// bit 13 = kCallStageStruct, bits 16-23 = k (the key of nested key 1), bit 14 = kCallAlphaFromF1 (row alpha = the call's f1 = MdlAlpha, the slot-32 submit), bit 15 = kCallStageOwn (the call was the slot-13 own submit; clear = a
// slot-32 / other pass), bits 24-25 = the guest LR class of a non-own call (kStageLr*), bit 31 = kCallStageCaster (stage_struct_nodes.h: the own submit's node is one the game's shadow passes draw; bits 30-31 are the
// specular code only on event / slot rows). The cage and the cell each have TWO nodes that submit the same models (an own and a slot-32 family); the nodes draw different sides / pieces each frame, so both families are
// recorded and drawn (S111 CAGESIDE review: weapon_plan.h, the comment at its top).
inline constexpr uint32_t kCallStageStruct = 0x2000, kCallAlphaFromF1 = 0x4000, kCallStageOwn = 0x8000;
inline constexpr uint32_t kStageLrShift = 24, kStageLrMask = 0x3;
inline constexpr uint32_t kStageLrNone = 0, kStageLrCage = 1, kStageLrCell = 2, kStageLrOther = 3;   // kStageLrNone = an own-submit call
inline constexpr uint32_t kLrCage36 = 0x826F9F08;                                   // [C] sub_826F9C40's slot-32 tail: the cage category-36 node (the palette on the stack is 2 rows)
inline constexpr uint32_t kLrCell1 = 0x8266118C, kLrCell2 = 0x82661460, kLrCell3 = 0x82661774;   // [C] sub_82660628's slot-32 submits (HIAC: +B60 / +E34 / +1148)
inline constexpr uint32_t kStageCagePaletteRows = 2;                                // [C] sub_826F9C40: the loop `r31 < 128` builds exactly 2 x 64 bytes
inline constexpr uint32_t kHolderGlobal = 0x837631C4, kHolderOffModel = 76, kHolderOffCount = 164, kHolderOffLoaded = 172, kHolderOffType = 176, kHolderMaxKeys = 18;   // [C] CAGE.md section 1 + Adversary 1
// Review fix 10: a stage row's `id` (and a stage section-59 row's) is kStageIdBase + PACH, never the bare PACH 0..7: an older reader that does not know bit 13 / kSlotFlagStage would take the id for an m2.pac weapon id (0..7 are real weapons) and
// draw a weapon at the cage's poses; 0x10000 + PACH is no m2 id, so it loads nothing. (The record version stays: the row layout is unchanged.)
inline constexpr int32_t kStageIdBase = 0x10000;
inline constexpr int32_t StageId(int32_t pach) { return kStageIdBase + pach; }
inline constexpr int32_t StagePachOfId(int32_t id) { return id - kStageIdBase; }   // negative / >= 16 = not a stage id
// Review fix 3: the guest caller of an OWN submit (the lr of the hook on sub_82C13C88 = who called the slot-13 dispatcher): 1 = the cage's category-26 node (sub_826FA0B8 .. before sub_826FA7C8 [L: the function range; the exact call site is
// not read]), 3 = any other caller (the CWeapon pieces' sub_825DFA78, the chamber's category-3 node, the Inferno: a third family / a double draw shows as an own-other count next to an own-cage one), 0 = the caller was not captured.
// S111 PODGLASS (docs\research\s111\PODGLASS.md; stage_node_state.h): 2 = kStageLrOwnNode, an own call made by a node that draws under ITS OWN state: the Elimination Chamber's category-35 / 36 draw sub_826F8F88
// (0x826F8F88 .. 0x826F9770, 2024 bytes [C listing]: blend code 0, alpha test GEQUAL 1, z write 0; the fences, the pod frames / doors / front wall at the fade, the pod glass k 5, the k 8 spotlight at code 1).
// The value 2 is kStageLrCell for a NON-own (slot-32) call; for an own call it is this class. OwnOtherCaller = the own-other family of the census and map B (any own caller but the cage node's): unchanged by the split.
// S111 CHAMBERSHADOW review: the class is read from the ONE caller -> node table (stage_struct_nodes.h kNodes), not from ranges of its own.
inline constexpr uint32_t kOwnChamberSeeNodeLo = 0x826F8F88, kOwnChamberSeeNodeHi = 0x826F9770;   // sub_826F8F88's range (podglass_unit's bounds; the table's ChamberSeeNode row, asserted below)
inline constexpr uint32_t kStageLrOwnNode = 2;
inline constexpr uint32_t OwnCallerClass(uint32_t caller) {
  if (caller == 0) return kStageLrNone;
  const stg_nodes::Node n = stg_nodes::NodeOfCaller(caller);
  return n == stg_nodes::Node::CageNode ? kStageLrCage : n == stg_nodes::Node::ChamberSeeNode ? kStageLrOwnNode : kStageLrOther;
}
static_assert(stg_nodes::NodeOfCaller(kOwnChamberSeeNodeLo) == stg_nodes::Node::ChamberSeeNode && stg_nodes::NodeOfCaller(kOwnChamberSeeNodeHi - 4) == stg_nodes::Node::ChamberSeeNode &&
                  stg_nodes::NodeOfCaller(kOwnChamberSeeNodeHi) != stg_nodes::Node::ChamberSeeNode,
              "kOwnChamberSeeNode* is the node table's ChamberSeeNode range");
inline constexpr bool OwnOtherCaller(uint32_t cls) { return cls == kStageLrOther || cls == kStageLrOwnNode; }
inline constexpr bool IsStageCall(uint32_t slot_flags) { return (slot_flags & kCallStageStruct) != 0; }
inline constexpr bool IsStageOwn(uint32_t slot_flags) { return (slot_flags & kCallStageOwn) != 0; }
inline constexpr uint32_t StageEntry(uint32_t slot_flags) { return (slot_flags >> kCallEntryShift) & kCallEntryMask; }
inline constexpr uint32_t StageLrClass(uint32_t slot_flags) { return (slot_flags >> kStageLrShift) & kStageLrMask; }
inline constexpr uint32_t LrClassOf(uint32_t lr) { return lr == kLrCage36 ? kStageLrCage : (lr == kLrCell1 || lr == kLrCell2 || lr == kLrCell3) ? kStageLrCell : kStageLrOther; }
// The holder's structure type [H+176] -> the bgEtc PACH of its path (the game requests a path, never a PACH: the table is ours). Type 4 (the ladder rig) has FIVE paths and is NOT mapped by type: its PACH comes from the path the
// request sub_82576768 was given (StagePathPach below, latched by the raw hook on that function). -1 = none / not mapped.
inline constexpr uint32_t kHolderTypeLadder = 4;
inline constexpr int32_t StagePachOfType(uint32_t type) { return type == 0 ? 0 : type == 1 ? 1 : type == 2 ? 2 : type == 3 ? 3 : type == 5 ? 6 : type == 7 ? 7 : -1; }
inline constexpr bool IsLadderPach(int32_t pach) { return pach == 4 || pach == 5 || (pach >= 11 && pach <= 61); }
// S103 LDR2 (docs\research\s103\LDC.md section 4 [C]): the ladder rig's holder models 2 / 3 (k 2 = light pool, k 3 = light shaft over the briefcase; sub_825DC218 -> [C+68] / [C+72]) are drawn by sub_825DB7B8 with blend code 1 (SRC_ALPHA, ONE: additive) under light key set 500.
inline constexpr bool IsLadderLightPiece(int32_t pach, uint32_t k) { return IsLadderPach(pach) && (k == 2 || k == 3); }
// The light key set of a ladder piece: the rope (k 0) and both lights (k 2 / 3) install set 500 (sub_825DB710, sub_825DB7B8 [C]); the belt / briefcase (k 1) is a CWeapon (sub_825DB328 -> sub_824D6C68, id 23) drawn by
// the CWeapon node under its own set 50 [C, S111 WEAPONLIGHT: weapon_light_key.h; rf_props.cpp CWeaponLights takes that row first] -> -1 here.
inline constexpr int32_t LadderLightKey(int32_t pach, uint32_t k) { return (IsLadderPach(pach) && (k == 0 || IsLadderLightPiece(pach, k))) ? 500 : -1; }
// S111 CHAMBERMAP (docs\research\s111\CHAMBERMAP.md, docs\systems\elimination_chamber.md [C]): the light key set of a stage structure piece, as the game's draw nodes install it. Cage / cell 100, Inferno 31, ladder LadderLightKey;
// the Elimination Chamber (holder type 5 = PACH 6 CHAI) installs set 35 before k 0 / k 1 and set 36 before k 2..8 (sub_826F8958 +0x44 / +0xAC, sub_826F8F88 +0x168 / +0x1CC). A set the manager lacks is not installed
// (sub_827F6A40 -> -1, the install skipped), so a chamber k >= 2 piece keeps the 35 installed before it: the fallback below. `has(id)` = the record carries set id; -1 = none (the caller's arena-props fallback).
// S111 CHAMBERMAP review [C listing + files]: the manager's set ids are the acts of the stage key it was given (sub_827F7BA0 +0x33C / +0x5E4; the stage load sub_824E8E38 passes bgNN.pac entry 30000), and no retail
// arena's key (all 57 bg pacs, no DLC arena) has act 35 or 36: in the shipped game both lookups miss and the chamber draws under the light its draw worker installed last (S111 CHAMBERLIGHT, docs\systems\elimination_chamber.md
// "The light the pieces really draw under": category 3 = the last character the rim pass drew [C chain], 35 / 36 = the arena's category-35 group `a` last run [L]); this pick returns -1 and the arena props set stands in:
// a known LOOK difference, not built (it needs the wrestler latch's block and the group-`a` run on the stage rows).
inline constexpr int32_t kChamberPach = 6;
template <class HasSet>
constexpr int32_t StageLightKeyOf(int32_t pach, uint32_t k, HasSet has) {
  const int32_t key = (pach >= 0 && pach <= 3) ? 100 : pach == 7 ? 31 : pach == kChamberPach ? (k <= 1 ? 35 : 36) : LadderLightKey(pach, k);
  if (key >= 0 && has(key)) return key;
  const int32_t fallback = (pach == kChamberPach && k >= 2) ? 35 : -1;
  return (fallback >= 0 && has(fallback)) ? fallback : -1;
}
inline constexpr int32_t StageTypeOfPach(int32_t pach) { return pach == 0 ? 0 : pach == 1 ? 1 : pach == 2 ? 2 : pach == 3 ? 3 : pach == 6 ? 5 : pach == 7 ? 7 : IsLadderPach(pach) ? int32_t(kHolderTypeLadder) : -1; }
// S103 LDR (docs\research\s103\COV10_match_objects.md B1; re-read from the listing of sub_82576DD8, 2026-11-19): the ladder holder's request passes ONE of five path strings in r4 to sub_82576768 (strings 0x8201D5EC .. 0x8201D61C [C q addr]):
// "/STG/LADM" -> bgEtc PACH 4, "/STG/LADW" -> 5, "/STGL/%04d" (formatted into the caller's stack buffer: "/STGL/0004") -> 11 + row, rows 0000-0045 = PACH 11-56, 0086-0090 = PACH 57-61 [C: the directory, bgetc_stg_structures.md];
// "/STG/LADU" / "/STG/LADC" have no bgEtc row [C] -> -1 (the game's load of them fails too). Every other path ("/STG/CAGE" ...) -> -1 as well: only type 4 asks. Pure; `n` <= 12 bytes of the guest string.
inline constexpr int32_t StagePathPach(const char* s, uint32_t n) {
  const auto at = [&](uint32_t i) -> char { return i < n ? s[i] : '\0'; };
  if (!(at(0) == '/' && at(1) == 'S' && at(2) == 'T' && at(3) == 'G')) return -1;
  if (at(4) == '/' && at(5) == 'L' && at(6) == 'A' && at(7) == 'D' && at(9) == '\0') return at(8) == 'M' ? 4 : at(8) == 'W' ? 5 : -1;
  if (at(4) == 'L' && at(5) == '/') {
    int32_t row = 0;
    for (uint32_t i = 6; i < 10; ++i) {
      const char c = at(i);
      if (c < '0' || c > '9') return -1;
      row = row * 10 + (c - '0');
    }
    if (at(10) != '\0') return -1;
    return row <= 45 ? 11 + row : (row >= 86 && row <= 90) ? 57 + (row - 86) : -1;
  }
  return -1;
}
// The plan's per-structure arrays (the family latch, the held rows, the counts) are indexed by PACH: bgEtc's STG rows reach 61 (the per-title ladder rigs).
inline constexpr int32_t kStagePachCap = 64;
// S103 SMX review A1: the stage id range only (kStageIdBase + PACH, PACH < kStagePachCap): the manager belts (BeltId, 0x300000+) and blood ids are other ranges and stay weapons.
inline constexpr bool IsStageRangeId(int32_t id) { return id >= kStageIdBase && id < kStageIdBase + kStagePachCap; }
inline constexpr uint32_t PackStageFlags(uint32_t k, bool own, bool alpha_from_f1, uint32_t lr_class) {
  return kCallStageStruct | kCallVtableOk | (own ? kCallStageOwn : 0u) | (alpha_from_f1 ? kCallAlphaFromF1 : 0u) | ((k & kCallEntryMask) << kCallEntryShift) | ((lr_class & kStageLrMask) << kStageLrShift);
}
inline constexpr uint32_t kSlotFlagStage = 4;   // a section-59 row of a live stage structure model (the drawer's prefetch): id = PACH, slot = k, main = the model
inline constexpr uint32_t kWantStage = 0x200;   // the drawer's (PACH, k | kWantStage) want pairs: a bgEtc.pac stage structure model, not an m2.pac / evtobj.pac one

// S103 GBR (docs\research\s103\GMS.md; docs\formats\gm_gmev.md "GMGA / GMGB model banks"; docs\systems\arena_look.md "Blood splats"): two more families recorded at the same raw hook, same row, no layout change (only bits that were reserved 0).
//  - kCallGmSet (bit 26): a GMSK set object (Create-a-Story backstage set gm.pac GMGB/78FR + 80FR, Elimination Chamber extras GMGA/0021): id = kGmIdBase + the object's key (u32 [obj+44], 1000..1755); the bank is the arena's (78 / 80 = GMGB, else GMGA)
//    and the drawer takes it from the record's arena. alpha = f1 = [model+252]; nodes = r5.
//  - kCallBloodSplat (bit 27): ONE blood splat (the pool's model [[0x83767DE4]+8], one call per live decal record): id = kBloodIdBase + the high 16 bits of the record's colour float (bfloat16: the r = g = b scalar of the material colour),
//    bits 16-23 = the texture index into the key-32010 list (blood01..03), alpha = f1, nodes = r5 (the world matrix the game built from the record's pos / angle / scale).
inline constexpr uint32_t kCallGmSet = 0x4000000, kCallBloodSplat = 0x8000000;
inline constexpr int32_t kGmIdBase = 0x100000, kGmIdSpan = 0x1000, kBloodIdBase = 0x200000;
inline constexpr bool IsGmCall(uint32_t slot_flags) { return (slot_flags & kCallGmSet) != 0; }
inline constexpr bool IsBloodCall(uint32_t slot_flags) { return (slot_flags & kCallBloodSplat) != 0; }
// S103 STG (docs\research\s103\PMW.md): a call made UNDER the game's shadow flag A (u32 [0x83929A48 + 4 * thread slot] != 0: inside a shadow-map render). The colour drawer never draws a tagged row; slice 2 reads them as the game's caster list.
//  - kCallShadowPass (bit 28): the call ran under flag A.   - kCallShadowProps (bit 29): and the thread's current camera was the props light (shadow module + 1312), not map A / B (module + 304).
inline constexpr uint32_t kCallShadowPass = 0x10000000, kCallShadowProps = 0x20000000;
inline constexpr bool IsShadowCall(uint32_t slot_flags) { return (slot_flags & kCallShadowPass) != 0; }
inline constexpr bool IsShadowPropsCall(uint32_t slot_flags) { return (slot_flags & kCallShadowProps) != 0; }
// S111 CHAMBERSHADOW review: the caster bit of a stage row shares no bit with anything a stage row carries (PackStageFlags, the shadow-pass tags, the gm / blood kinds).
static_assert((kCallStageCaster & (PackStageFlags(kCallEntryMask, true, true, kStageLrMask) | kCallShadowPass | kCallShadowProps | kCallGmSet | kCallBloodSplat | kCallEventProp)) == 0, "kCallStageCaster overlaps a stage row bit");
inline constexpr int32_t GmId(uint32_t key) { return kGmIdBase + int32_t(key & (kGmIdSpan - 1)); }
inline constexpr int32_t GmKeyOfId(int32_t id) { return id >= kGmIdBase && id < kGmIdBase + kGmIdSpan ? id - kGmIdBase : -1; }
// S111 GMBORROW (docs\research\s111\GMBORROW.md): bit 16 of a gm row (the entry field, unused on gm rows) = the game handed this object's controller the stage holder's texture set [[0x837631C4]] (settings byte
// G+41 set: sub_827C9190), not the stage's own list [[0x83763544]+14452]. The drawer loads the bank with the chamber's bgEtc PACH 6 set only then (asset_load.h LoadGmEntry `holder_tex`).
inline constexpr uint32_t kCallGmHolderTex = 1u << kCallEntryShift;
inline constexpr uint32_t PackGmFlags(bool holder_tex = false) { return kCallGmSet | kCallVtableOk | (holder_tex ? kCallGmHolderTex : 0u); }
inline constexpr bool GmHolderTex(uint32_t slot_flags) { return (slot_flags & kCallGmHolderTex) != 0; }
inline constexpr uint32_t PackBloodFlags(uint32_t tex) { return kCallBloodSplat | kCallVtableOk | ((tex & kCallEntryMask) << kCallEntryShift); }
inline float ColourOfBloodId(int32_t id) {   // the bfloat16 colour back as a float
  const uint32_t b = (uint32_t(id - kBloodIdBase) & 0xFFFFu) << 16;
  float f;
  __builtin_memcpy(&f, &b, 4);
  return f;
}
inline int32_t BloodIdOf(float colour) {
  uint32_t b;
  __builtin_memcpy(&b, &colour, 4);
  return kBloodIdBase + int32_t(b >> 16);
}
// S103 LDR (docs\research\s103\COV10_match_objects.md B2): the weapon manager's id 14 (the belt, nested one level deeper in m2.pac under a sub-key the game takes from the TITLE: SetWeaponData sub_8258FF90 0x82590094..0x825900D8) is recorded with
// id = kBeltIdBase + sub-key, so a call row carries which of the 51 belts it is (the m2 leaf p0/1000/14/<sub>: entry 0 model, 10 textures [C pac.py]; the game falls back to sub-key 0 when the key is absent: the loader does the same).
// A reader that does not know the base sees an unknown id (no m2 weapon is numbered 0x30xxxx): nothing draws, nothing breaks.
inline constexpr int32_t kWeaponIdBelt = 14, kBeltIdBase = 0x300000, kBeltIdSpan = 0x10000;
inline constexpr int32_t BeltId(uint32_t sub) { return kBeltIdBase + int32_t(sub & 0xFFFFu); }
inline constexpr int32_t BeltSubOfId(int32_t id) { return id >= kBeltIdBase && id < kBeltIdBase + kBeltIdSpan ? id - kBeltIdBase : -1; }
inline constexpr uint32_t kTitleMgrGlobal = 0x836F2D88, kTitleMgrOffKey = 189208, kSettingsGlobal = 0x8376AAF0, kSettingsOffTitleObj = 72428, kTitleObjOffKey = 8;   // [C] COV10 B2 (asm 0x82590094..0x825900D8)
inline constexpr uint32_t kGmListGlobal = 0x8376A9A0, kGmCtlCount = 12, kGmTableOff = 2120, kGmTableLen = 512, kGmObjOffModel = 48, kGmObjOffKey = 44;   // [C] GMS.md (A) Draw / Lists
// S111 GMBORROW [C sub_827C9190]: settings byte G+41 (G = 0x8376AAF4, sub_827D7578; 1 for match types 39 / 40, the Elimination Chamber) = byte 1 of this big-endian word; non-zero = the holder's set is handed
inline constexpr uint32_t kGmHolderTexWord = 0x8376AB1C, kGmHolderTexShift = 16;
inline constexpr uint32_t kBloodHolderGlobal = 0x83767DE4, kBloodPoolGlobal = 0x837689D4, kBloodHolderOffModel = 8, kBloodRecOff = 2448, kBloodRecStride = 48, kBloodRecCount = 30;   // [C] GMS.md (B) Pool / Set-up
inline constexpr uint32_t kBloodRecOffAlpha = 20, kBloodRecOffColour = 24, kBloodRecOffTex = 32;
// S111 BLOODMOD2 (docs\research\s111\BLOODMOD2.md): the pool's particles behind the records: particle k at Q + 48 + 80 k, +24 its state (0..3 alive, 4 free, 5 placed off the floor), +64 its alpha, which the pool
// update sub_826983B8 copies into record k while [Q + 3888] == 0; the replay track (vtable 0x82028CE0, slots 7 / 8) sets that word to 1 while its slot 3 writes the records itself.
inline constexpr uint32_t kBloodPartOff = 48, kBloodPartStride = 80, kBloodPartOffState = 24, kBloodPartOffAlpha = 64, kBloodStateFree = 4, kBloodPoolOffReplay = 3888;
// S111 BLOODMOD2 (the permanent blood mod only: floor_blood_switch.h On): a blood row's identity. slot_flags bits 0-7 = the record's particle slot k (CallSlot); `instance` = BloodIdentity(the pool count the row was
// recorded under, the splat's serial: scene_live\blood_slot_id.h), 0 = no identity (a record the replay track wrote, or one that is not its particle's own copy). Mod off: slot bits 0, instance = the n-th splat.
inline constexpr uint32_t BloodIdentity(uint32_t pools, uint32_t serial) { return (serial & 0xFFFFu) == 0 ? 0u : ((pools & 0xFFFFu) << 16) | (serial & 0xFFFFu); }
inline constexpr uint32_t BloodIdentityPools(uint32_t identity) { return identity >> 16; }
// S111 BLOODMOD3 (docs\research\s111\BLOODMOD3.md; the permanent blood mod only): kCallBloodHeld (bit 30) marks a blood row the tracker MADE from a live particle the game is fading without drawing its own record
// (an instant replay: scene_live\blood_held.h). It is keep-only: the store keeps it (floor_blood_keep.h Apply; the kept copy drops the bit) and the plan never draws it as itself (weapon_plan.h Plan).
inline constexpr uint32_t kCallBloodHeld = 0x40000000;
inline constexpr bool IsBloodHeld(uint32_t slot_flags) { return (slot_flags & kCallBloodHeld) != 0; }
static_assert((kCallBloodHeld & (PackBloodFlags(kCallEntryMask) | 0xFFu | kCallShadowPass | kCallShadowProps | kCallGmSet | kCallStageCaster)) == 0, "kCallBloodHeld overlaps a blood row bit");
// (review fix 1: above the key range 1000..1755 / 0x6DB: 0x400 collided with keys >= 1024)
inline constexpr uint32_t kWantGm = 0x10000, kWantBlood = 0x20000;   // the drawer's (arena, key | kWantGm) / (arena, texture | kWantBlood) asked-for pairs (gm / blood models are asked for at their first draw)
inline constexpr uint32_t WantGm(uint32_t key) { return key | kWantGm; }
inline constexpr uint32_t WantBlood(uint32_t tex) { return tex | kWantBlood; }
inline constexpr bool IsWantGm(uint32_t v) { return (v & kWantGm) != 0; }
inline constexpr uint32_t kWantGmHolderTex = 0x80000;   // S111 GMBORROW: + the gm row's kCallGmHolderTex (DropGbr drops the model it asked for: GmEntryOf's "/h" form). Review: 0x40000 is kWantRecoloured (below)
inline constexpr uint32_t WantGmHolder(uint32_t key, bool holder_tex) { return key | kWantGm | (holder_tex ? kWantGmHolderTex : 0u); }
inline constexpr bool IsWantGmHolderTex(uint32_t v) { return (v & kWantGmHolderTex) != 0; }
inline constexpr uint32_t WantKeyOf(uint32_t v) { return v & ~(kWantGm | kWantBlood | kWantGmHolderTex); }

inline constexpr uint32_t kEventWeaponVtable = 0x82029628;   // [C] the CWeapon made by MakeWeapon (sub_8272BCD0 / the dtor sub_8272D670 set it; BELT.md Adversary 4)
inline constexpr uint32_t kOffEventModels = 1228;            // [C] W + 1228 + 4i, i = 0..9: the models of one event CWeapon (BELT.md Adversary 1)
inline constexpr uint32_t kMaxEventModels = 10;
// S103 BLF review 4: an event prop model that MakeWeapon has made (still alive) is also a WeaponSlot row, so the drawer starts its load before the model's first draw: flags bit 1, id = the prop id, slot = k, main = the model.
// (Section 59 is census / prefetch only: the draw never joins on it.)
inline constexpr uint32_t kSlotFlagEvent = 2;
inline constexpr uint32_t kWantEvent = 0x100;   // the drawer's (id, entry) want pairs: entry | kWantEvent = an evtobj.pac event prop entry, not an m2.pac weapon model
inline constexpr uint32_t kSlotFlagRecoloured = 16;    // S111 FOLLOWUPS3: an event row whose model the game built from its recoloured read (scene_live\prop_read_note.h): the drawer asks for the "/s" entry
inline constexpr uint32_t kWantRecoloured = 0x40000;   // S111 FOLLOWUPS3: entry | kWantEvent | kWantRecoloured = that model's own evtobj.pac entry (weapon_draw_core.h EvtEntryOf "/s")
// S111 GMBORROW review: every want flag is its own bit (the gm / blood pairs and the event / stage pairs live in different lists today; one value for two meanings would merge them the day a list holds both).
static_assert((kWantGmHolderTex & (kWantGm | kWantBlood | kWantEvent | kWantStage | kWantRecoloured)) == 0, "kWantGmHolderTex shares a bit with another want flag");
// S111 CUTREACH2 review (docs\research\s111\CUTREACH2.md "Review"): section 59 serves two uses on two clocks. The weapon manager's table of the latch BEFORE the record's (the walk whose calls the record carries,
// latch_period.h) is the draw side: a plain row (ArenaFixedPropsDrawn counts it). A row of the NEWEST latch's table that the draw side does not hold is a load HINT only (flags bit 3): the drawer's prefetch asks
// for it so a model's load starts before its first draw; nothing that changes what is drawn counts it.
inline constexpr uint32_t kSlotFlagHint = 8;
inline constexpr uint32_t PackEventFlags(uint32_t k) { return kCallEventProp | kCallVtableOk | ((k & kCallEntryMask) << kCallEntryShift); }
inline constexpr uint32_t CallSlot(uint32_t slot_flags) { return slot_flags & 0xFFu; }
inline constexpr uint32_t CallSecondaryIndex(uint32_t slot_flags) { return (slot_flags >> kCallKShift) & kCallKMask; }
// Only the weapon's OWN submit (sub_82C13C88, vtable slot 13) becomes a row: the tracker marks it at its source (a REX_HOOK_RAW on sub_82C13C88 sets the thread's "slot-13 submit" model, draw_origin.h).
// r4 does NOT single it out (slot 19 sub_82C13A80 +1A4 and slot 32 sub_82C13EC8 also pass r4 = 0: PW review fix 4), so no r4 is recorded (bits 16-31 are reserved).
inline constexpr uint32_t PackSlotFlags(uint32_t slot, bool secondary, bool vtable_ok, uint32_t k = 0) {
  return (slot & 0xFFu) | (secondary ? kCallSecondary : 0u) | (vtable_ok ? kCallVtableOk : 0u) | (((secondary ? k : 0u) & kCallKMask) << kCallKShift);
}

}  // namespace rdr_scene::weapons
