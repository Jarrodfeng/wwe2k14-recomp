// S110 ENTRBLACK (docs\research\s110\ENTRBLACK.md "Round 3"): the game's own "this frame has scene geometry" decision, carried in SceneHeader::flags bits 1 / 2 (append-only: an old record has both 0 =
// not read = drawn as before). The file keeps its S105 A3 name; A3's camera words (the shown camera, the motion camera's anim, the phase) are gone: they were a proxy for this decision.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game or SDK includes.
//
// What the game does [C listings]:
//   * the stage: the loaded stage S = [0x83763544] is shown or hidden by sub_824EBF48(S, on): [S + 1176] = on (+10; its only writer: q field 1176 --writes) and, in the same call, the enable word +8 of its
//     draw nodes found by tag in S + 1184 ('SDL0' = the stage group of render category 22, 'SDTA', 'SDTR', 'CSDP', 'SDPC') and of [0x83924784]. The render walk sub_82C2FA10 skips a node whose +8 is 0.
//     The stage is built hidden (sub_824EDDB0 +310) and shown by the scene flow through sub_82577D30 -> sub_824EBF48: the entrance (sub_82330340 +1E8; sub_82327B78 -> sub_82314640(1)), the match flow
//     (sub_827B1ED8 +48, sub_827BD2A8 +30), the editors and the event scenes; hidden again by sub_82314640(0), match_state_reinit and the editors' exits.
//   * a character: drawn only when [char + 1208] (its opaque node's +8, sub_8251E510), [char + 224] and [char + 232] are non-zero (sub_823CBC50; inputs_contrib.h reads the same predicate per slot).
// So the game submits NO scene geometry on a frame whose stage word is 0 and whose characters are all hidden. Read on the latch thread, before the walk the game would make for this frame: the same
// frame. The camera composer is not part of it: sub_823C8548 hands its output slot on to the renderer every frame, stale or not (ENTRBLACK R1).
#pragma once
#include <cstdint>

namespace rdr_scene {

inline constexpr uint32_t kSceneFactRead = 2u;   // SceneHeader::flags bit 1 (bit 0 = kSceneStagePlaying, scene_record.h): a stage object exists and its show word was read at the latch
inline constexpr uint32_t kSceneDrawn = 4u;      // bit 2: the game draws scene geometry this frame (the stage is shown, or a character is)
inline constexpr uint32_t kSceneFactMask = kSceneFactRead | kSceneDrawn;   // (bits 3 / 4 were S105 A3's camera words: never written again)

#ifdef WWE2K14_HOST_PLANTS
inline int g_scene_fact_plant = 0;   // test only (tools\renderer\host_core_unit.cpp): 1 = the reader ignores the stage word (always shown); 2 = the reader ignores the characters
#define SCENE_FACT_PLANT(n) (::rdr_scene::g_scene_fact_plant == (n))
#else
#define SCENE_FACT_PLANT(n) false
#endif

inline constexpr uint32_t kLoadedStage = 0x83763544u, kStageShownOff = 1176u;   // S, [S + 1176]
inline constexpr uint32_t kCharTable = 0x836F38D0u, kCharSlotsN = 9u;          // the 9 character words (lightlatch::kCharSlotTable)
inline constexpr uint32_t kCharNodeOn = 1208u, kCharGateA = 224u, kCharGateB = 232u;

// The writer's bits for one latch.
inline constexpr uint32_t SceneFactBits(bool read, bool drawn) { return read ? (kSceneFactRead | (drawn ? kSceneDrawn : 0u)) : 0u; }
// The host's question: the record says the game submits no scene geometry this frame. A record that did not read the fact never says so.
inline constexpr bool GameDrawsNoScene(uint32_t header_flags) { return (header_flags & kSceneFactRead) != 0 && (header_flags & kSceneDrawn) == 0; }

inline constexpr bool ScenePtrValid(uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; }

// The reader over rw(guest address, uint32_t& value) -> bool (false = unreadable). read = a stage object exists and [S + 1176] was readable; no stage (0: the menus, an editor with none), a wild pointer or
// an unreadable word: read = false (never black on a failed read). drawn = the stage word is non-zero, or a character's predicate is true; a character whose words cannot be read counts as drawn.
template <class RW>
inline void ReadSceneFact(RW rw, bool& read, bool& drawn) {
  read = drawn = false;
  uint32_t s = 0, w = 0;
  if (!rw(kLoadedStage, s) || !ScenePtrValid(s) || !rw(s + kStageShownOff, w)) return;
  read = true;
  drawn = w != 0 || SCENE_FACT_PLANT(1);
  if (drawn || SCENE_FACT_PLANT(2)) return;
  for (uint32_t i = 0; i < kCharSlotsN && !drawn; ++i) {
    uint32_t ch = 0, a = 0, b = 0, c = 0;
    if (!rw(kCharTable + 4u * i, ch)) { drawn = true; break; }
    if (ch < 0x10000u) continue;   // an empty slot
    if (!rw(ch + kCharNodeOn, a) || !rw(ch + kCharGateA, b) || !rw(ch + kCharGateB, c)) { drawn = true; break; }
    drawn = a != 0 && b != 0 && c != 0;
  }
}

}  // namespace rdr_scene
