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
  // S103 RL F2: the per-light camera-attached flags (the u32 words at light object +1056 + 4 i, == 1 = the light's direction is VIEW space: V^T d; the game's own per-frame data, copied there from the
  // source's +8960 + 4 i by sub_827F70D0 / sub_827F8808 [C listings, docs\formats\light_set.md]). has_follow = the words were read (hook) or known (files: LightSet::follow).
  bool has_follow = false;
  bool follow[4] = {};
};
// Light::pad of a Directional light (slot 0-3) carries its set's flag for that light (S103 RL F2). A record from before this has pad == 0 = "not recorded": the reader falls back to the file's flags.
inline constexpr uint16_t kLightPadFollowKnown = 0x8000, kLightPadFollowOn = 0x0001;

// One stage slot as the arena's text (pac entry 50001) describes it.
struct SlotSnap {
  uint16_t slot = 0;
  uint32_t light_set = kNone;  // l(N): the text's set (100 when the row has none); kNone = no text row
  bool explicit_l = false;     // S103 A7: the text row HAS an l(N) (light_set 100 may be the loader's default, which no record proves)
  bool l_unread = false;       // S103 B4X F1: the row's last light token is an le( / lg( form (sub_824E8628), not understood: SlotLightSet = kNone
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
// through the entrances AND in every menu sequence below, the editor (seq 76, written at 0x83397CA0) included: S103 PHS, so the game's own slot look is the entrance one in the editor; docs/systems/match_state.md). [C listing; the live values 1 / 4: docs/systems/ai_decision.md, run 241.] Unreadable = Unknown.
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
  // S103 V1 (R17 [C]): per stage slot 0..398, 1 = the game HIDES it (no light install, no draw), read from the stage object's slot records at the latch (SlotShown). Empty = unread / unknown: nothing is hidden.
  std::vector<uint8_t> hidden;
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

// S103 A5: the stage slots the arena contributor EMITS as visible objects now: a slot with a row in the published list, minus the events-only slots in the match look and the game-only slots in the entrance look (Emit's own rule).
// An unknown look keeps every listed slot (asked, never dropped). false = no slot table published (`out` cleared): the caller then asks about every stage model. Sorted.
bool EmittedSlots(std::vector<uint16_t>& out);
// S103 V1 (R17 [C]): the game's per-slot draw predicate sub_824E7190 (0x824E71C8-0x824E71F0), pass-independent part, on one 12-byte slot record (stage + 1220 + 12 * slot): shown = (b9 & 0x80) && !(b8 & 0x80) && !(b9 & 0x08).
// A hidden slot gets neither a light install nor a draw. NOT applied (left as before): the byte +6 look gates (bit 1 is skipped when [0x836F3940] == 1 = the match look, 0x83420F8C [C, S103 EPS]; bit 2 with phase 1 needs [0x836F394C] / [0x835E1924], bit 4 [0x836F3970], bit 8 is skipped when phase != 1; the arena's own e / g look rule
// from the slot text stays the only look gate) and the slots 65-70 / 110-126 / 140-156 hide (sub_822BC3C0's test is open [U], R17).
inline constexpr uint32_t kStageSlotRecords = 399, kStageSlotRecordSize = 12, kStageSlotRecordsBase = 1220;
inline bool SlotShown(const uint8_t* rec) { return (rec[9] & 0x80) && !(rec[8] & 0x80) && !(rec[9] & 0x08); }
using SlotRecordReadFn = bool (*)(uint8_t* out, size_t n);   // reads kStageSlotRecords * 12 bytes at stage + 1220 (the glue wires stagefx); false = no stage / unreadable
void SetSlotRecordReader(SlotRecordReadFn fn);               // null = the old behaviour (nothing hidden)
void PlantIgnoreShown(bool on);                              // unit only: the pre-V1 twin (the bit is read but ignored)
uint32_t SlotLightSet(uint16_t slot, int32_t stage_arena, bool* is_explicit = nullptr);   // S103 A7 / B4X: the slot text's l(N), or the loader default 100 for a row without one and for a slot below 399 with no row (sub_824E8E38 fills slot words 0..398); *is_explicit = the text names it (a default never replaces a fresh learned entry); kNone (0xFFFFFFFF) for a row with an le( / lg( token, a slot >= 399 with no row, no published table, or a table of another arena
void MovingSlots(std::vector<uint16_t>& out);                         // S103 C2: fills `out` with the published slots that have an m(N) motion
int32_t PublishedArena();                                             // S103 C2: the published arena id, -1 unknown
int PublishedLookCode();                                              // S103 M: the published arena look as 0 unknown / 1 match / 2 entrance
bool ScreenShown(uint16_t key);                                      // S103 M (B2): the published look shows stage screen `key` (slot 800-809): in the slot table and not hidden by its e / g flag in this look (look unknown = shown, the conservative answer)
// (was: the stage-fx writer's list)                               // S103 C2: the published slots with an m(N) motion (the stage-fx writer's list)
Contributor* Instance();                                            // the "arena" contributor (static lifetime)

// Registration is one call from the live writer's startup (src\renderer_adapter\live.cpp, main session). Inline so the hooks and tools that
// only Publish do not need the writer linked.
inline void RegisterArena() { Register(Instance()); }

#ifdef RDR_CROWD_IDS_PLANTS
// S102 T11b, test only (tools\crowd\crowd_ids_test.cpp): a planted fault in the arena's pool fields. 0 none; 1 the pac string's field is kept without the arena id (stale after an arena change);
// 2 the mirror twin's id (":r") answers with the slot's own id field (the suffix lost); 3 the "slot N" entry's field is kept without N.
inline int g_arena_strings_plant = 0;
#endif

// The census the arena keeps for its own tests (total over all Emit calls).
struct Totals {
  uint64_t emits = 0, objects = 0, unknown = 0, stale = 0, sets = 0, twins = 0, screens = 0;
};
Totals GetTotals();

}  // namespace rdr_scene::live::arena
