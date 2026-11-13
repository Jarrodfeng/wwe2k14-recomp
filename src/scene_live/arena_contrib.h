// The arena as a live scene contributor (Session 101, lane 28, class C3; docs\systems\arena_look.md "What is live", docs\renderer\SCENE_RECORD.md
// "The live writer"). No game or SDK includes: the game hooks (arena_hooks.h) and the offline tool (tools\scene\arena_rec.cpp) both fill an
// ArenaSnapshot; Emit turns it into the arena's part of the scene record.
//
// What a record carries for the arena (ids as the game numbers them; unknown stays unknown and is counted):
//  - the arena pac as one AssetRef per stage slot (pac "pac\bg\bgNN.pac", entry "slot K"), one Object per slot ("arena:slot:K"), plus the
//    50001 text's twins: "arena:slot:K:r" (kObjMirrorX, world = diag(-1,1,1)) and "arena:slot:K:y" (kObjTurned, world = diag(-1,1,-1));
//    Object::layer 1 for the slots the text blends (a / s), 0 otherwise; Object::light_set = the set id the slot's l(N) names (the game's
//    own number, NOT an index; 0xFFFFFFFF = the slot has no text row), Object::params = "arena.slot_motion" (m(N)) / "arena.slot_when"
//    (bit 1 = e, events only; bit 2 = g, game only) when set.
//  - the evaluated light sets (Light table): per set id, slots 0..3 (and 4 where the source gave a fifth) = Directional (dir = the stored
//    direction as the game keeps it, colour = the evaluated colour; Light::range = the cube falloff where known, else -1 = unknown), slot 5 = Ambient
//    with the colour of the light object's block at +400 and slot 6 = Ambient with the block at +416 (sky / ground order [U], see
//    docs\formats\light_set.md).
//  - a slot listed under an unknown look (e / g rows) is NOT visible and carries ParamValue "arena.visibility_unknown" (counted).
//  - the screen surfaces (stage slots 800-809): one Target (role TronPicture, index = key - 800) and one Movie (OnTarget) each; the plane
//    image ids are 0xFFFFFFFF = unknown, counted (the plane -> surface binding is not traced: docs\systems\video_tron.md).
// Not carried (each counted or named in arena_look.md): the entrance rig's slot remap of the sets, the camera key (+300), the follow spot,
// pyro, ropes (class C5).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

#include "../renderer_scene/live_scene.h"

namespace rdr_scene::live::arena {

inline constexpr uint32_t kNone = 0xFFFFFFFFu;
// Light::range of a directional light whose cube falloff is not known (the live object does not hold it): -1. 0 is a real falloff; readers keep the
// asset's own value for a negative range (review F1).
inline constexpr float kUnknownRange = -1.f;

// One light object's evaluated values (what sub_827F8730 left in it this frame: after the key, the blend and the +452 brightness scale).
struct SetSnap {
  uint32_t set_id = kNone;
  uint8_t n_lights = 4;        // directional lights present (4, or 5 when the source read the fifth)
  float dir[5][3] = {};
  float col[5][3] = {};
  float block_a[3] = {};       // light object +400 (sky [U])
  float block_b[3] = {};       // light object +416 (ground [U])
  bool has_fall = false;
  float fall[4] = {};          // the 896-byte kind's +0x140 cube falloff (from the files; not on the live object)
};

// One stage slot as the arena's text (pac entry 50001) describes it.
struct SlotSnap {
  uint16_t slot = 0;
  uint32_t light_set = kNone;  // l(N): the text's set (100 when the row has none); kNone = no text row
  uint8_t mirror = 0;          // 1 = r (drawn also mirrored in the YZ plane), 2 = y (also turned 180 degrees about Y)
  uint8_t blend = 0;           // 1 = a (additive), 2 = s (translucent)
  uint8_t when = 0;            // 1 = e (events only), 2 = g (game only)
  int32_t motion = -1;         // m(N): the slot follows stage-key motion N, -1 = none
};

struct ScreenSnap {
  uint16_t key = 800;          // stage slot 800-809
  uint32_t planes[4] = {kNone, kNone, kNone, kNone};  // decoded Y / cR / cB / A image ids, kNone = unknown
};

enum class Look : uint8_t { Unknown = 0, Match = 1, Entrance = 2 };

// S101 lane 264: the game's own look decision. The stage-slot draw (vtable method sub_83420EA0, listing 0x83420F30-0x83420FFC) tests the slot record's byte +6 (parsed by
// CStage::ModelStatusSetup sub_824E8628: `e` = bit 1, `g` = bit 8) against ONE live boolean, r31 = ([0x836F3940] == 1): an `e` slot is skipped while it is true, a `g` slot while
// it is false. So the look is the scene phase word every frame (1 = the match, anything else = the entrance look; the word is 1 at boot and in the match scene, 4 from match load
// through the entrances: docs/systems/match_state.md). [C listing; the live values 1 / 4: docs/systems/ai_decision.md, run 241.] Unreadable = Unknown.
inline constexpr uint32_t kScenePhaseWord = 0x836F3940;
inline Look LookFromScenePhase(bool readable, uint32_t word) {
  if (!readable) return Look::Unknown;
  return word == 1 ? Look::Match : Look::Entrance;
}

struct ArenaSnapshot {
  int32_t arena_id = -1;       // pac\bg\bgNN.pac; -1 = unknown
  Look look = Look::Unknown;   // which slots are drawn: the match look hides e-only slots, the entrance look g-only slots
  float brightness = 1.f;      // the manager's +452 (already applied to the blocks)
  std::vector<SetSnap> sets;
  std::vector<SlotSnap> slots;
  std::vector<ScreenSnap> screens;
  uint32_t dropped_sets = 0;   // light objects the hook could not read this frame (counted unknown)
  uint32_t partial_sets = 0;   // sets read without their fifth light
};

// ---- the file side (pure functions) -------------------------------------------------------------------------------------------------
// Parses the slot text (pac entry 50001, the lines `NNN ... l(N) ... r y a s e g m(N) ;`) exactly as src\assets\asset_load.h does.
// Rows come back in slot order. A row without l(N) has light_set 100 (what the loader assumes).
std::vector<SlotSnap> ParseSlotText(std::string_view text);

// ---- the contributor ------------------------------------------------------------------------------------------------------------------
// Hooks / tools publish; Emit (writer thread) copies the latest under a short lock.
void PublishAll(const ArenaSnapshot& s);                              // replace the whole snapshot
void PublishSets(int32_t arena_id, float brightness, std::vector<SetSnap>&& sets, uint32_t dropped = 0, uint32_t partial = 0);  // the per-frame part (the hook): keeps slots / screens
void PublishSlots(int32_t arena_id, Look look, std::vector<SlotSnap>&& slots, std::vector<ScreenSnap>&& screens);  // the static part
void PublishLook(Look look);                                          // S101 lane 264: the per-frame look (the hook reads the scene phase word); keeps slots / screens / sets
bool NeedsSlots(int32_t arena_id);                                    // true when the slot table is not published for this arena yet

Contributor* Instance();                                              // the "arena" contributor (static lifetime)

// Registration is one call from the live writer's startup (src\renderer_adapter\live.cpp, main session). Inline so the hooks and tools that
// only Publish do not need the writer linked.
inline void RegisterArena() { Register(Instance()); }

// The census the arena keeps for its own tests (total over all Emit calls).
struct Totals {
  uint64_t emits = 0, objects = 0, unknown = 0, stale = 0, sets = 0, twins = 0, screens = 0;
};
Totals GetTotals();

}  // namespace rdr_scene::live::arena
