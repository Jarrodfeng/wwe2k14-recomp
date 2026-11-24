// The weapon drawer's PLAN (S103 lane PW-3 + the PW-1/2/3 integration, contract src\renderer_scene\weapon_rows.h, evidence docs\research\s103\PW_adversary.md). Header only, no GPU, no game, no SDK, no asset code: the
// applier (rf_props.cpp ApplyWeaponRows), the pose half (weapon_draw_core.h) and the offline units (tools\scene\weapon_draw_unit.cpp, tools\weapons\weapon_combined_test.cpp) all run THESE functions.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// WHAT IT DOES: a record's sections 60 (one WeaponCall per call of a weapon model, copied at the call) and 61 (their node rows) say what the game drew; Plan() turns them into items, one per CALL:
//   - the NEWEST frame present in the record only (the contributor already writes only the newest closed frame's calls, weapon_rows_contrib.h; an older call in a hand-made / old record is `late`, counted, not drawn: PW-1/2 review fix 2);
//   - the id comes from the call itself (`call.id`: the table of the call's own frame; no join across a reload: review fix 5), -1 = unknown;
//   - only the weapon's own submit is recorded (the slot-13 dispatcher marks it at its source, draw_origin.h; r4 does NOT single it out: slot 19 / 32 also pass 0): the drawer takes every row it is given;
//   - the model entry: 0 = the slot's main model, 1 / 2 = its secondary model k = 0 / 1 (slot_flags bits 8 + 10-11): a secondary is its own mesh with its own pose slot (review fix 3);
//   - S103 LDR: the ring steps (ids 29 / 32 / 35: the game stores the arena's own colour in the slot id, sub_825DDAB0) are DRAWN from the rows, with the commentary desk (6 / 7); the arena loader's fixed copies are not drawn in the live path (rf_props.cpp). The belts (id 14: nested one level deeper in m2.pac, sub-key = the title) are skipped and counted until LDR step 5.
// Section 59 (the slot table) is not needed to draw: the call carries its id; it stays in the record for the census and the viewer.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "scene_record.h"
#include "weapon_rows.h"
#include "rows_ring.h"   // S106 G4-C: RingNodeRows (sizeof in WeaponViewOf)

namespace rdr_scene::weapons {

// S103 CGF: which of the cage's two submit families the drawer takes when a structure has both (docs\research\s103\CAGE.md section 3 item 3, Adversary 3 B): 0 = the own-submit family (category-26 node, slot 13), 1 = the slot-32 family
// (category-36 node, f1 = MdlAlpha). Which one reaches the colour picture is [U]; the game side sets this from --wwe2k14_rdr_stage_struct_family=own|slot32 (wwe2k14_app.h). Never both.
inline uint32_t g_stage_family = 0;
// S103 LDR (review fix 2): decided PER RECORD, not per process. The weapon manager's rows draw the ring steps (the arena's own variant 29 / 32 / 35) and the commentary desk (6 / 7), so the arena loader's fixed copies of them are hidden
// whenever the record carries a weapon table (section 59 has rows); a record with none (an offline rig without the weapon rows) keeps them. `g_force_arena_fixed_props` (--wwe2k14_rdr_arena_fixed_props, a diagnostic) draws both always.
// The arena model keeps the meshes either way: their light set is what the weapons are lit with.
inline bool g_force_arena_fixed_props = false;
inline bool ArenaFixedPropsDrawn(size_t record_weapon_slots) { return g_force_arena_fixed_props || record_weapon_slots == 0; }
inline constexpr uint32_t kStageFamilyOwn = 0, kStageFamilySlot32 = 1;
// Review fix 6: the family chosen for a PACH is LATCHED (kept by the drawer's per-cache state) until that structure is gone (its PACH leaves the holder's section-59 rows) or the PACH changes: a frame whose own calls vanish
// ([model+560] != 0: the cached command-buffer branch never reaches sub_82C04DF0) must not flip a cage between opaque and MdlAlpha. A latched family with no rows in a frame draws nothing that frame (counted).
// S103 GBR (CGF follow-up): a latched family with no rows in a frame is HELD: the last frame's rows of that structure are drawn again (up to kStageHoldMax frames) instead of nothing (no blink, no family flip);
// HoldStageRows below makes the plan's input. Held frames are counted (Census::Stage::held).
inline constexpr uint32_t kStageHoldMax = 30;
struct HeldStage {
  std::vector<WeaponCall> calls;   // the last frame's usable rows of the latched family (node_first rebased to `nodes`)
  std::vector<float> nodes;        // 12 floats per node
  uint32_t age = 0;                // frames the rows have been held in a row
  uint64_t sig = 0;                // review 10: the structure's member set (section-59 stage rows of this PACH: slot k + model address) when the snapshot was taken; a hold needs the SAME set now
};
struct StageFamilyLatch {
  int32_t fam[kStagePachCap];
  HeldStage held[kStagePachCap];
  StageFamilyLatch() { for (int32_t& f : fam) f = -1; }
  void Reset(int32_t pach) {
    if (pach >= 0 && pach < kStagePachCap) fam[pach] = -1, held[pach] = HeldStage();
  }
};
// Review fix 4: a slot-32 row (alpha from f1) with alpha not > 0 (or NaN) is not drawn; its alpha is clamped to 1. The drawer blends such a row (kObjForceBlend) so MdlAlpha 0.16 shows as 0.16.
inline bool StageRowSkip(const WeaponCall& w) { return (w.slot_flags & kCallAlphaFromF1) != 0 && !(w.alpha > 0.f); }
inline float StageRowAlpha(const WeaponCall& w) { return (w.slot_flags & kCallAlphaFromF1) != 0 && w.alpha > 1.f ? 1.f : w.alpha; }
inline bool StageRowForceBlend(const WeaponCall& w) { return (w.slot_flags & kCallAlphaFromF1) != 0; }

inline constexpr bool IsBeltsId(int32_t id) { return id == 14; }   // nested deeper in m2.pac, sub-key [L]: skipped and counted

// What a record's weapon sections look like to the drawer: plain pointers (the record's vectors, or a test's arrays). empty() = nothing to draw and nothing to pay.
struct View {
  const WeaponSlot* slots = nullptr;   // section 59 (census / viewer only: the drawer needs no join)
  size_t n_slots = 0;
  const WeaponCall* calls = nullptr;
  size_t n_calls = 0;
  const float* nodes = nullptr;   // 12 floats per node (section 61)
  size_t n_nodes = 0;
  bool empty() const { return n_calls == 0 || !calls || !nodes || n_nodes == 0; }
};

// The record's three weapon tables as a View. A template on the table type so a unit can hand it the real SceneTables; the member names and element types are checked at COMPILE time (a rename in the contributors
// breaks this build, never silently draws nothing: the PW-3 draft detected them with a `requires` and an empty View).
template <class T>
inline View WeaponViewOf(const T& t) {
  static_assert(sizeof(typename std::decay_t<decltype(t.weapon_calls)>::value_type) == sizeof(WeaponCall) && sizeof(typename std::decay_t<decltype(t.weapon_nodes)>::value_type) == 12 * sizeof(float) &&
                    sizeof(typename std::decay_t<decltype(t.weapon_slots)>::value_type) == sizeof(WeaponSlot),
                "SceneTables::weapon_slots / weapon_calls / weapon_nodes are the contract's rows (32 / 32 / 48 bytes)");
  View v;
  v.slots = t.weapon_slots.data(), v.n_slots = t.weapon_slots.size();
  v.calls = t.weapon_calls.data(), v.n_calls = t.weapon_calls.size();
  v.nodes = reinterpret_cast<const float*>(t.weapon_nodes.data());
  v.n_nodes = t.weapon_nodes.size();
  return v;
}

// Planted twins (the offline units set them; the game never does).
struct Plants {
  bool draw_steps = true;         // S103 LDR: ids 29 / 32 / 35 are drawn from the rows (the game's own variant); false = the planted twin: the old skip (the arena loader's fixed copy drew a always-grey pair)
  bool merge_instances = false;   // twin: two calls of one id are drawn as one (the old dedupe on the object)
  bool transpose_rows = false;    // twin: the node rows are read transposed (the 3x3 part)
  bool ignore_frame = false;      // twin: every frame of the record is drawn (an older frame's calls are drawn again)
  bool secondary_as_main = false; // twin: a secondary model is drawn with the main mesh of its id (the PW integration's bug, review fix 3)
  uint32_t stage_family = g_stage_family;   // S103 CGF: the preferred submit family of a stage structure (kStageFamily*)
  StageFamilyLatch* latch = nullptr;   // review fix 6: the drawer's per-cache latch (null = no latching: the unit's one-shot plans)
  bool ignore_latch = false;      // twin (review fix 6): the family is re-chosen every frame
  bool stage_both = false;       // twin (S103 CGF): both submit families of a structure are drawn (the cage drawn twice, once opaque and once at MdlAlpha)
  bool hold_ignore_sig = false;  // twin (S103 GBR review 10): a structure is held across a frame where its member set (holder) changed
  bool no_hold = false;          // twin (S103 GBR): a latched family with no rows draws nothing (the old behaviour: the structure blinks out for the frame)
  bool stage_id_plain_ok = false;   // twin (S103 SMX): a stage id without the stage flag goes to the m2 weapon path (one failed m2.pac load per structure run)
  bool gm_dead_alpha = false;    // twin (S103 GBR): a gm set object / blood splat row with alpha not > 0 is drawn (opaque) instead of skipped
};

struct Census {
  uint32_t calls = 0;          // WeaponCall rows seen
  uint32_t drawn = 0;          // items drawn (set by the applier after the model is posed; Plan counts the items it returns in `planned`)
  uint32_t planned = 0;
  uint32_t unknown_id = 0;     // a call with no id (-1: its object was in no slot at its call)
  uint32_t stage_id_plain = 0, stage_id_plain_flags = 0;   // S103 SMX: calls with a stage id (kStageIdBase + PACH in the stage range; never a belt 0x300000+ or a blood id) and no stage flag (refused, also counted in unknown_id) and the first one's slot_flags
  uint32_t no_model = 0;       // an item whose model could not be had (not resident / load failed): counted, the rest of the frame is drawn (review fix 2)
  uint32_t prefetched = 0;     // (id, entry) pairs of section 59 asked for before the plan loop (review fix 2)
  uint32_t skipped_steps = 0;  // ids 29 / 32 / 35 (the arena loader draws them)
  uint32_t belts = 0;          // id 14 calls (not loaded)
  uint32_t bad_rows = 0;       // a call whose node rows run past section 61
  uint32_t late = 0;           // calls of a frame older than the newest in the record (not drawn)
  uint32_t shadow_rows = 0;    // S103 STG: calls tagged kCallShadowPass (made under the game's flag A): the game's caster submits, never drawn by the colour drawer
  uint32_t event_items = 0;    // S103 BLF: event prop calls planned (belts / mics / caps / stage props of an entrance)
  uint32_t stage_items = 0;    // S103 CGF: stage structure calls planned
  uint32_t stage_other_family = 0;   // S103 CGF: stage calls of the family NOT drawn (counted, never drawn: the cage's two nodes submit the same models)
  uint32_t gm_items = 0, blood_items = 0;   // S103 GBR: gm set object / blood splat calls planned
  uint32_t gm_drawn = 0, blood_drawn = 0;   // S103 GBR: set by the applier after the model is posed
  uint32_t gm_dead = 0, blood_dead = 0;    // S103 GBR: rows skipped for alpha not > 0 (a splat the game would not draw / an invisible set object)
  // `[rdr][weapons] gm set N items (alpha0 skipped A); blood splat M items (alpha0 skipped B)` (the step check reads it)
  std::string GbrLine() const {
    char b[200];
    std::snprintf(b, sizeof b, "[rdr][weapons] gm set %u items (alpha0 skipped %u); blood splat %u items (alpha0 skipped %u)", gm_items, gm_dead, blood_items, blood_dead);
    return b;
  }
  struct Stage {               // S103 CGF: the census of the stage structure rows of the newest frame (StageLine)
    int32_t pach = -1;
    uint32_t models = 0, calls = 0, slot32 = 0, own = 0, other_lr = 0, drawn = 0, own_cage = 0, own_other = 0, alpha0 = 0, latched_empty = 0, held = 0;   // own_cage / own_other: own calls by the slot-13 caller (review 3); alpha0: slot-32 rows skipped (alpha not > 0); latched_empty: calls dropped because the latched family had no rows
    float f1_min = 0.f, f1_max = 0.f;
    int32_t family = -1;       // the family drawn (kStageFamily*), -1 = none
    bool operator==(const Stage& o) const { return pach == o.pach && models == o.models && calls == o.calls && slot32 == o.slot32 && own == o.own && other_lr == o.other_lr && drawn == o.drawn && family == o.family; }
  } stage;
  // `stage type T pach P models N calls C families: slot32 A own B (f1 min / max), drawn family F` (the step check reads it: which family reaches the picture is [U]; other-LR = a call of a stage model from neither the cage nor the cell node)
  std::string StageLine() const {
    char b[520];
    std::snprintf(b, sizeof b, "[rdr][weapons] stage type %d pach %d models %u calls %u families: slot32 %u own %u (own: cage-node %u other-caller %u; f1 min %.3f / max %.3f; other-LR calls %u; alpha0 skipped %u), drawn family %s (%u drawn, latched-empty %u, held %u)", StageTypeOfPach(stage.pach), stage.pach,
                  stage.models, stage.calls, stage.slot32, stage.own, stage.own_cage, stage.own_other, double(stage.f1_min), double(stage.f1_max), stage.other_lr, stage.alpha0,
                  stage.family == int32_t(kStageFamilyOwn) ? "own" : stage.family == int32_t(kStageFamilySlot32) ? "slot32" : "none", stage.drawn, stage.latched_empty, stage.held);
    return b;
  }
  std::vector<int32_t> ids;    // the distinct ids drawn, ascending
  void NoteDrawn(int32_t id) {
    ++drawn;
    const auto it = std::lower_bound(ids.begin(), ids.end(), id);
    if (it == ids.end() || *it != id) ids.insert(it, id);
  }
  std::string Line() const {
    std::string s = "[rdr][weapons] drawn " + std::to_string(drawn) + " (ids";
    if (ids.empty()) s += " none";
    for (int32_t i : ids) s += " " + std::to_string(i);
    s += "), unknown id " + std::to_string(unknown_id) + ", no model " + std::to_string(no_model);
    if (skipped_steps || belts || bad_rows || late)
      s += "; skipped steps " + std::to_string(skipped_steps) + " belts " + std::to_string(belts) + " bad rows " + std::to_string(bad_rows) + " late " + std::to_string(late);
    if (event_items) s += "; event props " + std::to_string(event_items);
    if (stage_id_plain) { char b[96]; std::snprintf(b, sizeof b, "; stage id without the stage flag %u (first slot_flags 0x%X)", stage_id_plain, stage_id_plain_flags); s += b; }
    return s;
  }
};

struct Item {
  int32_t id = -1;
  uint32_t call = 0;       // index into View::calls
  uint32_t instance = 0;   // WeaponCall::instance
  uint32_t entry = 0;      // the model: 0 = main, 1 / 2 = secondary k = 0 / 1 (the m2.pac leaf entry; EntryOf in weapon_draw_core.h)
  bool event = false;      // S103 BLF: an EVENT PROP (entrance belt / mic / cap / stage prop): `id` = the prop id, `entry` = the PACH entry k of pac\evt\evtobj.pac (EvtEntryOf), not an m2.pac weapon
  bool stage = false;      // S103 CGF: a STAGE STRUCTURE piece (cage / cell / chamber / Inferno): `id` = the bgEtc PACH, `entry` = k, the key of nested key 1 (StgEntryOf in weapon_draw_core.h)
  bool gm = false;         // S103 GBR: a GMSK set object: `id` = `entry` = its key in the arena's gm.pac bank (GmEntryOf)
  bool blood = false;      // S103 GBR: one blood splat: `id` = kBloodIdBase + bfloat16 colour, `entry` = the texture index (key 32010's list)
};

// The m2.pac leaf entry of a call's model: 0 = the slot's main model, 1 + k = its secondary model k [L: k -> key k+1].
inline uint32_t EntryOfCall(const WeaponCall& w) { return (w.slot_flags & kCallSecondary) ? 1u + CallSecondaryIndex(w.slot_flags) : 0u; }

// S103 MBC (RMP.md 2.3 site 1, PMC.md section 1 / 4): does the game cast this item into the wrestlers' shadow map (map B, mode 0)? Categories 26 / 41 hold every CWeapon-class object; each node decides per object (the
// per-id skips below, asm 0x825DDE38..E9C for manager weapons and sub_8272D378 +48..+78 for event props [C PMC]). Ids 59 / 68 / 71 / 96 (parked in 32 until a weapon method moves them to 26) and 'BKIE' weapons are CAST and counted
// (`cast_unsure`: our row knows the model, not the weapon's current category [PMC missing 2]); id 135 casts: its extra test on [0x836F394C] (sub_825DFA78 +B0..+C8) sits OUTSIDE the flag-A branch, so a recorded row already means the word was non-zero [C MBC review 2].
// Ids 2064 / 2556 are moved to category 43 by sub_827FBFE0 (entrances): they cast like any prop but NOT when the scene word is 1 (piece43, the family bit kObjMapBPiece43; RecordShadow skips them then).
// Never: gm sets (categories 23 / 36), blood splats (the node returns under flag A), a flag-A shadow row, a stage row that is not the piece's own CWeapon submit of the "other" class (slot-32 rows and the cage node do not run under flag A).
inline constexpr bool IsMapBWeaponSkipId(int32_t id) {
  return id == 6 || id == 7 || id == 29 || id == 31 || id == 32 || id == 34 || id == 35 || id == 37 || (id >= 104 && id <= 107);
}
inline constexpr bool IsMapBEventSkipId(int32_t id) { return id == 2065 || id == 2114 || id == 2511 || id == 2555 || id == 2621 || id == 2642; }
inline constexpr bool IsMapBPiece43Id(int32_t id) { return id == 2064 || id == 2556; }   // S103 MBC review 6
inline constexpr bool IsMapBUnsureId(int32_t id) { return id == 59 || id == 68 || id == 71 || id == 96; }
inline bool CastsMapB(const Item& it, const WeaponCall& w, bool plant_cast_gm = false) {   // plant_cast_gm: the planted twin `mapb_casts_gm` (gm sets and blood cast): tools only
  if (IsShadowCall(w.slot_flags)) return false;
  if (!plant_cast_gm && (it.gm || it.blood || IsGmCall(w.slot_flags) || IsBloodCall(w.slot_flags))) return false;
  if (it.stage) return IsStageOwn(w.slot_flags) && StageLrClass(w.slot_flags) == kStageLrOther;
  if (it.event) return !IsMapBEventSkipId(it.id);
  return !IsMapBWeaponSkipId(it.id);
}

// S103 PM2 (PMC.md 1 / 3 / 4, PMW.md, RMP.md 2): does the game cast this item into the PROPS map (the floor's shadow map, sub_827E7F00)? The pass draws render categories 24 (gated by [0x836F39A0]: never in normal play),
// 26 and 41 (both gates constant), under flag A, so each node applies the same per-object skips as in map B (CastsMapB: weapon / event skip ids, stage own + "other" class, never gm / blood / a flag-A row). It differs from
// map B in exactly two ways [C PMC]: (1) it is NOT mode-gated (map B's mode 1 = scene word 1 with >= 3 participants casts no props; the props pass always draws 26 + 41), and (2) it never draws category 33 / 43:
// ids 2064 / 2556 are re-keyed to 43 by sub_827FBFE0 (entrances), so they cast into map B (outside scene word 1) but NOT into the props map. Pure; map B's filter is not bent (this calls it, then removes the piece-43 ids).
inline bool CastsPropsMap(const Item& it, const WeaponCall& w, bool plant_cast_gm = false) {
  if (it.event && IsMapBPiece43Id(it.id)) return false;
  return CastsMapB(it, w, plant_cast_gm);
}

// S103 PM2 review 1 (MBC review 5's cost rule, one predicate for rf_props.cpp and the rig fixture): is the posed CPU copy of a prop kept this frame? Only when a map will read it: SHADOWS on, and either map B takes
// props (mode 0: the scene word is known, and word 1 needs a known count < 3) or the props map does (the props-casters switch AND the props map switch are on; no mode gate). Otherwise the copy is dropped.
inline bool PropCpuWanted(bool casts_b, bool casts_p, bool props_map_on, bool shadows, int64_t phase_word, int32_t participants) {
  if (!shadows) return false;
  const bool mode0 = phase_word >= 0 && !(phase_word == 1 && (participants < 0 || participants >= 3));
  return (casts_b && mode0) || (casts_p && props_map_on);
}

// What section 59 asks the loader for, before anything draws (review fix 2): one (id, entry) per slot id (main) and per secondary model. Skips what the drawer never draws (steps, belts) and a duplicate.
inline std::vector<std::pair<int32_t, uint32_t>> Prefetch(const View& v, const Plants& p) {
  std::vector<std::pair<int32_t, uint32_t>> out;
  const auto add = [&](int32_t id, uint32_t entry) {
    if (id < 0 || (!p.stage_id_plain_ok && IsStageRangeId(id)) || (IsRingStepsId(id) && !p.draw_steps) || IsBeltsId(id)) return;   // S103 SMX: a stage id is never an m2 weapon (stage-flagged slots take the branch above, event slots are not asked through add)
    const std::pair<int32_t, uint32_t> k{id, entry};
    if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
  };
  for (size_t i = 0; v.slots && i < v.n_slots; ++i) {
    if (v.slots[i].flags & kSlotFlagStage) {   // S103 CGF: a live stage structure model (PACH, k | kWantStage): asked for before its first draw; never the steps / belts skips (the bgEtc id space)
      const int32_t spach = StagePachOfId(v.slots[i].id);   // the row's id is kStageIdBase + PACH (review 10)
      const std::pair<int32_t, uint32_t> k{spach, (v.slots[i].slot & kCallEntryMask) | kWantStage};
      if (spach >= 0 && spach < kStagePachCap && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
      continue;
    }
    if (v.slots[i].flags & kSlotFlagEvent) {   // S103 BLF review 4: an event prop model MakeWeapon has made: (prop id, entry k | kWantEvent), asked for before its first draw; never the steps / belts skips (another id space)
      const std::pair<int32_t, uint32_t> k{v.slots[i].id, (v.slots[i].slot & kCallEntryMask) | kWantEvent};
      if (v.slots[i].id >= 0 && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
      continue;
    }
    add(v.slots[i].id, 0);
    for (uint32_t k = 0; k < kMaxSecondary; ++k)
      if (v.slots[i].secondary[k]) add(v.slots[i].id, 1u + k);
  }
  return out;
}

// The newest guest frame (low 32 bits, wrap-safe) among the calls.
inline uint32_t NewestFrame(const View& v) {
  uint32_t newest = v.calls[0].frame;
  for (size_t i = 1; i < v.n_calls; ++i)
    if (int32_t(v.calls[i].frame - newest) > 0) newest = v.calls[i].frame;
  return newest;
}

// S103 GBR (CGF follow-up, StageFamilyLatch): the plan's input with the HELD rows added. Per latched structure (latch.fam[pach] >= 0): when the newest frame has usable rows of the latched family, they are copied into the latch
// (the snapshot, age 0); when it has none and a snapshot exists AND the structure's member set (section-59 stage rows) is unchanged (review 10),
// CAUSE of such a frame [L, not [C]]: NOT the [model+560] cached-buffer branch (no writer sets [model+560] non-zero in the index: q field 560 shows only zero-inits and the reads, and slot 19 sub_82C13A80, the one recorder of that buffer, is skipped whole under OURS,
// scene_skip_nodes.h) but a record that carries the stage section-59 rows without a newly closed guest frame (weapon_rows_contrib.h Emit: `if (!any) return` after the slot rows are written) or a guest frame with no call of the structure;
// the census `held N` says how often it happens; `held 0` over a match = the hold is dead code and goes. If a latched family has no rows (and a snapshot exists), the snapshot's rows are appended to a COPY of the view (stamped
// with the newest frame) for up to kStageHoldMax frames in a row, counted in Census::Stage::held; past that the snapshot is dropped (the structure really has no rows). Returns `in` itself when nothing was held (no copy),
// else a View over calls_buf / nodes_buf (the caller keeps both alive for the frame). A null latch or a twin (ignore_latch / no_hold / stage_both) = no hold and no snapshot.
inline View HoldStageRows(const View& in, const Plants& p, std::vector<WeaponCall>& calls_buf, std::vector<float>& nodes_buf, Census& c) {
  if (!p.latch || p.ignore_latch || p.no_hold || p.stage_both) return in;
  StageFamilyLatch& L = *p.latch;
  bool any_latched = false;
  for (int32_t f : L.fam) any_latched = any_latched || f >= 0;
  if (!any_latched) return in;
  const bool have = !in.empty();
  const uint32_t newest = have ? NewestFrame(in) : 0u;
  std::vector<uint32_t> usable[kStagePachCap];   // call indexes of the newest frame's usable rows of the latched family, per PACH
  for (size_t i = 0; have && i < in.n_calls; ++i) {
    const WeaponCall& w = in.calls[i];
    const int32_t pach = StagePachOfId(w.id);
    if (IsShadowCall(w.slot_flags) || !IsStageCall(w.slot_flags) || w.frame != newest || pach < 0 || pach >= kStagePachCap || L.fam[pach] < 0) continue;
    if (size_t(w.node_first) + size_t(w.node_count) > in.n_nodes || w.node_count == 0 || StageRowSkip(w)) continue;
    if ((IsStageOwn(w.slot_flags) ? kStageFamilyOwn : kStageFamilySlot32) != uint32_t(L.fam[pach])) continue;
    usable[pach].push_back(uint32_t(i));
  }
  // review 10: the member set of each structure NOW = its section-59 stage rows (slot k + the model's address): a holder that changed (the structure freed / made again, another type) gives another set, and the held rows are dropped
  uint64_t member_sig[kStagePachCap] = {};
  for (size_t i = 0; in.slots && i < in.n_slots; ++i) {
    if (!(in.slots[i].flags & kSlotFlagStage)) continue;
    const int32_t pach = StagePachOfId(in.slots[i].id);
    if (pach < 0 || pach >= kStagePachCap) continue;
    member_sig[pach] = (member_sig[pach] ^ (uint64_t(in.slots[i].main) << 8 ^ in.slots[i].slot ^ 0x100u)) * 1099511628211ull + 1;
  }
  std::vector<WeaponCall> add_calls;
  std::vector<float> add_nodes;
  for (int32_t pach = 0; pach < kStagePachCap; ++pach) {
    if (L.fam[pach] < 0) continue;
    HeldStage& h = L.held[pach];
    if (!p.hold_ignore_sig && !h.calls.empty() && h.sig != member_sig[pach]) h = HeldStage();   // the structure changed (or is no longer listed): nothing is held across that frame
    if (!usable[pach].empty()) {   // snapshot
      h.calls.clear(), h.nodes.clear(), h.age = 0, h.sig = member_sig[pach];
      for (uint32_t i : usable[pach]) {
        WeaponCall w = in.calls[i];
        const float* src = in.nodes + size_t(w.node_first) * 12;
        w.node_first = uint32_t(h.nodes.size() / 12);
        h.nodes.insert(h.nodes.end(), src, src + size_t(w.node_count) * 12);
        h.calls.push_back(w);
      }
    } else if (!h.calls.empty()) {
      if (h.age >= kStageHoldMax) { h.calls.clear(), h.nodes.clear(); continue; }   // held long enough: the structure really has no rows
      ++h.age;
      ++c.stage.held;
      const uint32_t base = uint32_t(in.n_nodes + add_nodes.size() / 12);   // this structure's snapshot indexes start at 0: shifted past the view's own nodes and the nodes added before it
      for (WeaponCall w : h.calls) {
        w.node_first += base;
        if (have) w.frame = newest;
        add_calls.push_back(w);
      }
      add_nodes.insert(add_nodes.end(), h.nodes.begin(), h.nodes.end());
    }
  }
  if (add_calls.empty()) return in;
  calls_buf.assign(in.calls, in.calls + in.n_calls);
  nodes_buf.assign(in.nodes, in.nodes + in.n_nodes * 12);
  calls_buf.insert(calls_buf.end(), add_calls.begin(), add_calls.end());
  nodes_buf.insert(nodes_buf.end(), add_nodes.begin(), add_nodes.end());
  View out = in;
  out.calls = calls_buf.data(), out.n_calls = calls_buf.size();
  out.nodes = nodes_buf.data(), out.n_nodes = nodes_buf.size() / 12;
  return out;
}

// The skips and the id. One item per call (never one per object).
inline std::vector<Item> Plan(const View& v, const Plants& p, Census& c) {
  std::vector<Item> out;
  c.calls += uint32_t(v.n_calls);
  if (v.empty()) return out;
  const uint32_t newest = NewestFrame(v);
  std::vector<int32_t> seen;   // merge twin only
  // S103 CGF: ONE submit family per structure per frame. The cage's two draw nodes (category 36 slot 32 / category 26 slot 13) submit the same models; both are recorded and tagged, the drawer takes the preferred family of each PACH
  // when it has a usable row, else the other (the chamber and the Inferno submit own only). Never both (the planted twin stage_both draws both).
  uint32_t stage_own[kStagePachCap] = {}, stage_slot32[kStagePachCap] = {};
  for (size_t i = 0; i < v.n_calls; ++i) {
    const WeaponCall& w = v.calls[i];
    const int32_t pre_pach = StagePachOfId(w.id);
    if (IsShadowCall(w.slot_flags) || !IsStageCall(w.slot_flags) || (w.frame != newest && !p.ignore_frame) || pre_pach < 0 || pre_pach >= kStagePachCap) continue;   // S103 STG: a flag-A call is no colour row
    if (size_t(w.node_first) + size_t(w.node_count) > v.n_nodes || w.node_count == 0 || StageRowSkip(w)) continue;
    ++(IsStageOwn(w.slot_flags) ? stage_own : stage_slot32)[pre_pach];
  }
  const auto stage_pick = [&](int32_t pach) -> uint32_t {   // the family drawn for this PACH
    const bool use_latch = p.latch && !p.ignore_latch;
    if (use_latch && p.latch->fam[pach] >= 0) return uint32_t(p.latch->fam[pach]);   // review 6: latched
    const bool has_own = stage_own[pach] > 0, has_32 = stage_slot32[pach] > 0;
    const uint32_t pick = p.stage_family == kStageFamilySlot32 ? (has_32 || !has_own ? kStageFamilySlot32 : kStageFamilyOwn) : (has_own || !has_32 ? kStageFamilyOwn : kStageFamilySlot32);
    if (use_latch && (has_own || has_32)) p.latch->fam[pach] = int32_t(pick);
    return pick;
  };
  uint32_t stage_k_seen[kStagePachCap] = {};   // bitmask of the keys (k < 32) seen per PACH
  for (size_t i = 0; i < v.n_calls; ++i) {
    const WeaponCall& w = v.calls[i];
    if (IsShadowCall(w.slot_flags)) { ++c.shadow_rows; continue; }   // S103 STG (PMW.md): made under the game's shadow flag A: a caster submit, never a colour row (before the frame test: never counted late)
    if (w.frame != newest && !p.ignore_frame) { ++c.late; continue; }
    if (IsGmCall(w.slot_flags) || IsBloodCall(w.slot_flags)) {   // S103 GBR: a gm set object / a blood splat: one item per call; alpha not > 0 (an invisible object / a dead record) is skipped and counted
      const bool blood = IsBloodCall(w.slot_flags);
      const int32_t key = blood ? w.id : GmKeyOfId(w.id);
      if (key < 0 || (blood && (w.id < kBloodIdBase || w.id >= kBloodIdBase + 0x10000))) { ++c.unknown_id; continue; }
      if (size_t(w.node_first) + size_t(w.node_count) > v.n_nodes || w.node_count == 0) { ++c.bad_rows; continue; }
      if (!(w.alpha > 0.f) && !p.gm_dead_alpha) { ++(blood ? c.blood_dead : c.gm_dead); continue; }
      ++(blood ? c.blood_items : c.gm_items);
      Item it;
      it.id = key, it.call = uint32_t(i), it.instance = w.instance, it.entry = blood ? StageEntry(w.slot_flags) : uint32_t(key), it.gm = !blood, it.blood = blood;
      out.push_back(it);
      continue;
    }
    if (IsStageCall(w.slot_flags)) {   // S103 CGF: a stage structure piece; every call is its own instance (the cage's wall is drawn 3 times)
      const int32_t pach = StagePachOfId(w.id);
      if (pach < 0 || pach >= kStagePachCap) { ++c.unknown_id; continue; }
      if (size_t(w.node_first) + size_t(w.node_count) > v.n_nodes || w.node_count == 0) { ++c.bad_rows; continue; }
      Census::Stage& st = c.stage;
      if (st.pach != pach) { const uint32_t held = st.held; st = Census::Stage(), st.pach = pach, st.held = held; }   // (S103 GBR: the held count was counted before the plan named the structure)   // one structure type at a time (the holder holds one type)
      const bool own = IsStageOwn(w.slot_flags);
      const uint32_t fam = own ? kStageFamilyOwn : kStageFamilySlot32;
      ++st.calls, ++(own ? st.own : st.slot32);
      if (own) {
        const uint32_t oc = StageLrClass(w.slot_flags);   // review 3: the slot-13 caller's class
        if (oc == kStageLrCage) ++st.own_cage;
        else if (oc == kStageLrOther) ++st.own_other;
      } else {
        st.f1_min = st.slot32 == 1 ? w.alpha : std::min(st.f1_min, w.alpha), st.f1_max = st.slot32 == 1 ? w.alpha : std::max(st.f1_max, w.alpha);
        if (StageLrClass(w.slot_flags) == kStageLrOther) ++st.other_lr;
      }
      const uint32_t ek = StageEntry(w.slot_flags);
      if (ek < 32 && !(stage_k_seen[pach] & (1u << ek))) stage_k_seen[pach] |= 1u << ek, ++st.models;
      if (StageRowSkip(w)) { ++st.alpha0; continue; }   // review 4: alpha not > 0 (or NaN): nothing of it shows
      if (!p.stage_both) {
        const uint32_t pick = stage_pick(pach);
        if (fam != pick) {
          ++c.stage_other_family;
          if ((pick == kStageFamilyOwn ? stage_own : stage_slot32)[pach] == 0) ++st.latched_empty;   // the latched family has no rows this frame: nothing is drawn (never a flip)
          continue;
        }
      }
      st.family = int32_t(fam);
      ++c.stage_items;
      out.push_back({pach, uint32_t(i), w.instance, ek, false, true});
      continue;
    }
    const int32_t id = w.id;
    const bool event = IsEventCall(w.slot_flags);   // S103 BLF: the prop id space (2000+) is not the weapon manager's: the steps / m2 belts skips below never apply to it
    if (id < 0) { ++c.unknown_id; continue; }
    if (!event && !p.stage_id_plain_ok && IsStageRangeId(id)) {   // S103 SMX (HOL2 SM): a stage id (kStageIdBase + PACH) without the stage flag is no m2 weapon: asking m2.pac for it is one failed load per structure run; counted, and the first call's slot_flags kept so the next run settles which row carries it
      if (c.stage_id_plain++ == 0) c.stage_id_plain_flags = w.slot_flags;
      ++c.unknown_id;
      continue;
    }
    if (!event && IsRingStepsId(id) && !p.draw_steps) { ++c.skipped_steps; continue; }
    if (!event && IsBeltsId(id)) { ++c.belts; continue; }
    if (size_t(w.node_first) + size_t(w.node_count) > v.n_nodes || w.node_count == 0) { ++c.bad_rows; continue; }
    if (event) {   // one item per call, never merged / deduped on the id (two models of one prop = two calls); its model is evtobj.pac PACH-of(id) entry k
      ++c.event_items;
      out.push_back({id, uint32_t(i), w.instance, EventEntry(w.slot_flags), true});
      continue;
    }
    if (p.merge_instances) {
      if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
      seen.push_back(id);
    }
    out.push_back({id, uint32_t(i), w.instance, p.secondary_as_main ? 0u : EntryOfCall(w)});
  }
  c.planned += uint32_t(out.size());
  return out;
}

// S103 LDR: the COMPANION census of the families the stream-free build used to miss or double-draw: per class, the rows the record carries in its newest frame ("recorded"), the items the plan made of them ("planned") and the models
// the applier pushed ("drawn"). A class where recorded > drawn is a hole; drawn > recorded a double draw. Pure (the offline test and the applier run the same code). Classes: the ladder rig (a stage row of a ladder PACH 4 / 5 / 11-61: rope +
// hanging belt / briefcase), the weapon manager's belt (id 14 under the title's sub-key), the ring steps by variant (29 / 32 / 35, the game's own colour), the commentary desk (6 / 7 / 100 / 105-107) and the backstage-brawl set (WPBS-only ids).
enum CovClass : uint32_t { kCovLadder = 0, kCovBelt, kCovSteps29, kCovSteps32, kCovSteps35, kCovDesk, kCovWpbs, kCovN };
inline int CovClassOfWeaponId(int32_t id) {
  if (BeltSubOfId(id) >= 0) return kCovBelt;
  switch (id) {
    case 29: return kCovSteps29;
    case 32: return kCovSteps32;
    case 35: return kCovSteps35;
    case 6: case 7: case 100: case 105: case 106: case 107: return kCovDesk;
    case 17: case 51: case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 61: case 62: case 64: case 68: case 69: return kCovWpbs;   // [C] COV10 C2: the ids that exist only in WPBS (m2 p1)
    default: return -1;
  }
}
inline int CovClassOfCall(const WeaponCall& w) {
  if (IsEventCall(w.slot_flags) || IsGmCall(w.slot_flags) || IsBloodCall(w.slot_flags)) return -1;
  if (IsStageCall(w.slot_flags)) return IsLadderPach(StagePachOfId(w.id)) ? int(kCovLadder) : -1;
  return CovClassOfWeaponId(w.id);
}
inline int CovClassOfItem(const Item& it) {
  if (it.event || it.gm || it.blood) return -1;
  if (it.stage) return IsLadderPach(it.id) ? int(kCovLadder) : -1;
  return CovClassOfWeaponId(it.id);
}
struct Coverage {
  uint32_t rec[kCovN] = {}, planned[kCovN] = {}, drawn[kCovN] = {};
  int32_t ladder_pach = -1;   // the ladder rig's PACH seen in the rows (-1 none)
  uint32_t lights_additive = 0;   // S103 LDR2: the ladder light pieces (k 2 / 3) drawn additive this frame (LDC.md)
  bool operator==(const Coverage& o) const {
    for (uint32_t c = 0; c < kCovN; ++c)
      if (rec[c] != o.rec[c] || planned[c] != o.planned[c] || drawn[c] != o.drawn[c]) return false;
    return ladder_pach == o.ladder_pach && lights_additive == o.lights_additive;
  }
  bool Any() const {
    for (uint32_t c = 0; c < kCovN; ++c)
      if (rec[c] || planned[c] || drawn[c]) return true;
    return false;
  }
  // `[rdr][weapons] coverage: ladder pach P rec A planned B drawn C | belt ... | steps v29 ... v32 ... v35 ... | desk ... | wpbs ...` (the step check reads it; prints on a change and every 600th frame it is non-empty)
  std::string Line() const {
    static const char* const kNames[kCovN] = {"ladder", "belt", "steps v29", "steps v32", "steps v35", "desk", "wpbs"};
    std::string s = "[rdr][weapons] coverage (recorded / planned / drawn):";
    char b[96];
    for (uint32_t c = 0; c < kCovN; ++c) {
      if (c == kCovLadder) std::snprintf(b, sizeof b, " %s%s%d %u/%u/%u", kNames[c], ladder_pach >= 0 ? " pach " : " pach ", ladder_pach, rec[c], planned[c], drawn[c]);
      else std::snprintf(b, sizeof b, " | %s %u/%u/%u", kNames[c], rec[c], planned[c], drawn[c]);
      s += b;
    }
    std::snprintf(b, sizeof b, " | lights additive %u", lights_additive);   // S103 LDR2
    s += b;
    return s;
  }
};
// S103 STG (PMW.md section 3 (e)): TEST-ONLY ORACLE. The call indexes of the newest frame's rows made under the game's shadow flag A: props_only = the props map's (kCallShadowProps), else any shadow map's. In a frame the game walks these are its exact
// caster submits (poses at the call); in an OURS frame the list is empty by construction (the game never submits under flag A there), so slice 2 must not depend on it: it checks its rule-built caster list against this one.
inline std::vector<size_t> ShadowCasterRows(const View& v, bool props_only) {
  std::vector<size_t> out;
  if (v.n_calls == 0 || !v.calls) return out;
  const uint32_t newest = NewestFrame(v);
  for (size_t i = 0; i < v.n_calls; ++i) {
    const WeaponCall& w = v.calls[i];
    if (w.frame == newest && (props_only ? IsShadowPropsCall(w.slot_flags) : IsShadowCall(w.slot_flags))) out.push_back(i);
  }
  return out;
}
// The rows of the newest frame of the view, by class (the plan's own newest-frame rule: the highest frame number in the record).
inline void CountRecorded(const View& v, Coverage& cv) {
  if (v.n_calls == 0) return;
  const uint32_t newest = NewestFrame(v);
  for (size_t i = 0; i < v.n_calls; ++i) {
    const WeaponCall& w = v.calls[i];
    if (w.frame != newest || IsShadowCall(w.slot_flags)) continue;   // S103 STG: a flag-A row is not a colour row (recorded > drawn would be a false hole)
    const int c = CovClassOfCall(w);
    if (c < 0) continue;
    ++cv.rec[c];
    if (c == kCovLadder) cv.ladder_pach = StagePachOfId(w.id);
  }
}
inline void CountPlanned(const std::vector<Item>& items, Coverage& cv) {
  for (const Item& it : items) {
    const int c = CovClassOfItem(it);
    if (c >= 0) ++cv.planned[c];
  }
}

}  // namespace rdr_scene::weapons
