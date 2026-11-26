// S111 STAGELESS2 (docs\research\s111\STAGELESS2.md): a 3D scene the GAME draws with no stage (Create a Title's belt over the red backdrop, sequence 29; the Threads editor's body).
// Pure header, no game / SDK includes: the record's header bit, READY's no-stage term, the scene boundary and the never_fed mask. Unit: tools\scene\stageless_unit.cpp (stageless_check.py).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game's condition [C]: [0x83763544] is its stage holder (written by the bgNN build sub_825E6078 +100, the created-arena build sub_8342A598, zeroed by the loader's free sub_827E4FC8 0x827E51CC:
// CACBG). Create a Title frees the stage and builds none (sub_83328CA8 -> sub_827E41E0), then draws the belt (a MakeWeapon prop at categories 26 / 33) in its own scene pass. A frame is a stageless 3D
// scene exactly when the game holds no stage AND a model-class hook asked this record (the Scene3D stamp's walk bit, scene_kind.h kWalkBit; never set in the stream build). With a stage held (a
// match, an entrance, CAS's BG66) or an unreadable holder the old rule stands: a record with no stage is a scene change (S103 ONF review M2) and blocks READY.
#pragma once
#include <cstdint>

namespace rdr_scene::stageless {

// SceneHeader::flags bit 6 (bit 0 kSceneStagePlaying, 1 / 2 cam_gate.h, 3 / 4 the retired S105 A3 pair, 5 rows_snapshot.h kSceneSnapshot): the latch read [0x83763544] and it was 0.
// An unreadable word leaves it clear (fail closed: the old rule). Written by inputs_contrib.h (Latch reads, Emit ORs); read by live_scene_draw.cpp (NoteStage) and scene_host.cpp (never_fed).
inline constexpr uint32_t kSceneNoStageHeld = 64u;
inline constexpr uint32_t NoStageHeldBits(bool no_stage_held) { return no_stage_held ? kSceneNoStageHeld : 0u; }

// Test only (tools\scene\stageless_unit.cpp): 1 = the old rule (no stage always blocks READY); 2 = the walk is not asked (a no-stage menu would be a scene); 3 = the stage-only inputs stay required.
inline int g_plant = 0;

// The gate, stepped ONCE per record (LiveSceneDraw::NoteStage). Open while the record says "no stage held" and a walk was seen within the last kRecords records: one record missing its walk bit
// mid-scene is no scene end (the host's own WalkGate::kRecords rule, scene_kind.h); kRecords records with no walk close it (the belt freed: the menu after Create a Title).
struct StagelessGate {
  static constexpr uint32_t kRecords = 3;
  uint32_t run = kRecords;   // records since the last walk (starts closed)
  bool Step(uint32_t header_flags, bool walked) {
    if ((header_flags & kSceneNoStageHeld) == 0 || g_plant == 1) return run = kRecords, false;
    if (walked || g_plant == 2) run = 0;
    else if (run < kRecords) ++run;
    return run < kRecords;
  }
};

// READY's no-stage term (live_scene_draw.cpp BuildTerms): a record with no stage blocks unless it is a scene the game draws with none.
inline constexpr bool NoStageBlocks(bool has_stage, bool stageless_scene) { return !has_stage && !stageless_scene; }

// The structural inputs whose ONLY producer is a stage (host_core.h kFeedStructural minus the camera 12 and the palette 3): 5 arena_worlds, 6 ring_rows, 14 stage_screens, 15 stage_pools, 16 arena_models
// (scene_inputs::Input, inputs_core.h; the unit checks the numbers). With no stage held nothing can feed them, so the never_fed hold must not wait on them (it would hold for its whole kMaxHold).
inline constexpr uint32_t kStageOnlyStructural = (1u << 5) | (1u << 6) | (1u << 14) | (1u << 15) | (1u << 16);
inline uint32_t RequiredInScene(uint32_t required, bool no_stage_held) { return (no_stage_held && g_plant != 3) ? (required & ~kStageOnlyStructural) : required; }

}  // namespace rdr_scene::stageless
