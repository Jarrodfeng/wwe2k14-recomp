// The scene inputs: every value the game's logic sets at run time that our frame needs, captured outside the scene walks (S103 lane A; docs\renderer\INPUTS_BOARD.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only; includes only our own pure headers (no game, SDK or renderer): the contributor (inputs_contrib.h), the game glue
// (inputs_glue.h) and the offline unit test (tools\scene\inputs_unit.cpp) all include this file.
//
// One bit per input of rdr_own::Pass::Scene3D (scene_frame.h). ALL of them are registered at start (RegisterAll): the decision answers InputsMissing, and the game walks, until every one carries a stamp.
// An input with no reader outside the walks yet (moved = false in kInputs) simply never gets one, so landing 1 stays off until the INPUTS_BOARD.md row is built: a registered-but-uncaptured
// input is the safe state, never a guess.
//
// LAG (S103 CLK2, FRAME_CONTRACT.md 3). A scene hook asking in period g (the clock reads g) decides frame g (walk-time hooks: g - 1, already decided in period g - 1). The first decider of frame f
// runs in period f BEFORE latch(f), so the newest latch it can see is latch(f - 1), which stamps hook label f - 2: a latch input is declared lag 2 (Decide(f) accepts a stamp in [f - 2, f]). sphere_occ is
// stamped by the update thread with its own frame (sphere_glue.h): lag 1 (f - 1, or f already when the update hook ran first). A reader that stops stamping leaves the gate at InputsMissing two frames later.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../renderer_game/scene_frame.h"
#include "../task_group_walk.h"  // S103 FDP: the one task-group walk
#include "stage_clock_flag.h"   // S103 E5: bit 17 (OFF)
#include "ring_stamp_flag.h"   // S103: bit 6 (released)
#include "caster_stamp_flag.h"   // S103 L1C: bit 7 moved follows it
#include "light_stamp_flag.h"    // S103 A4: bit 4 stamp switch (OFF)
#include "../renderer_scene/scene_record.h"
#include "post_settings.h"
#include "safe_guest_pages.h"   // BH3-4: ModelAlphaPlausible

namespace scene_inputs {

// The bit numbers are the ids the record's kScInputStamps carries: append only.
enum Input : int {
  kInPostSettings = 0,    // the post settings object S (kScPostParams)
  kInModelRuntime = 1,    // per wrestler model: MdlAlpha [model+252] (kScModelRuntime)
  kInSphereOcc = 2,       // the wrestlers' sphere occluders (kScSphereOcc; the update hook on sub_826DF8E0 already sits outside the walks)
  kInWrestlerPalette = 3, // the consumer skin palette (skeleton+200) as the draw reads it (kScBones)
  kInLightStates = 4,     // R, the render-camera light record each model call reads (kScLightStates / kScLightRefs)
  kInArenaWorlds = 5,     // the arena's evaluated object worlds and the stage clock (kScArenaWorlds)
  kInRingRows = 6,        // the ring's per-call node rows (kScRingRows / kScRingNodes)
  kInCasterEntries = 7,   // the shadow / rim / plane pass entries: camera state, palette, pass id (kScCasterCalls / States / Nodes / Extras / Mats)
  kInCirclePoints = 8,    // the circle shadows' body points and constants (kScCircleShadows)
  kInPlaneConstants = 9,  // the plane shadow chain's per-frame and per-draw constants (kScPlaneFrames / kScPlaneDraws)
  kInCrowdSnapshot = 10,  // the crowd node's classifier and snapshot copies (OWN_THE_PASS.md 8; lane D)
  kInParticles = 11,      // particle emitters' per-frame values (OWN_PARTICLES_NODE.md; lane F)
  kInCamera = 12,         // the frame's render camera: view, projection, eye, near / far (kScCamera, id 2)
  kInMaterialParams = 13, // S103 A2 (W4): the WRESTLER models' run-time parameter override entries (kScParamOverrides)
  kInStageScreens = 14,   // S103 M (B2): every stage screen the look shows has a picture row of its expected size this frame (stage_fx_contrib.h Emit; reason "screens")
  kInStagePools = 15,     // S103 L2: the entrance page's floor pools + follow spot (kSlPool / kSlSpot rows of kScStageLook). Stamped on every frame that is NOT an entrance, and on an entrance frame only when the rows exist
  kInArenaModels = 16,    // S103 A3 (A2 / A4): the ARENA models' MdlAlpha and override entries (own bit: an arena failure must not cost the wrestlers' stamps, review F4)
  kInStageClock = 17,     // S103 A3 (F5, coordinator decision): c186, the stage clock the UV-scrolling surfaces read. NOT built (moved = false): J1 proved c186 is never frame - origin; until its source is shown frames
                          // whose UV-scrolling surfaces need a clock stay the game's instead of going OURS with Time = 0
  kInputCount
};

// S103 A3 (F2 1): a consumer of the kLoModelMap rows exists (lane E3 / L reads the per-node install map). FALSE until it does: the light_states input (bit 4) is then never stamped and every frame stays the
// game's, because OURS frames with a map nobody reads would light every wrestler wrongly. The rows are still written (so the consumer can be developed). Set to true by the consumer's merge (L2's kStagePoolRowsExist
// pattern); a variable, not a constant, only so the unit can flip it.
inline bool g_light_map_consumer = true;   // S103 L1C: TRUE, but NOTE (review F4): no draw-side reader of kLoModelMap exists; this flag only adds a gate to the bit 4 stamp, it does not make a consumer. Wrestlers are lit from their own state (E8 / A8), arena models from the record's light rows (learned + A7 by id). --wwe2k14_rdr_scene_hold=4 re-holds bit 4
// S103 A4: the STAMP of bit 4 (light_states): the run-time copy of lightstamp::kStampLightStates (light_stamp_flag.h, OFF, release condition there). The latch stamps only with BOTH this and g_light_map_consumer.
inline bool& g_stamp_light_states = rdr_scene::live::lightstamp::g_on;

struct Info {
  int bit;
  const char* name;
  int lag;            // 2 = read at the latch (the decision accepts the stamp of f - 2 .. f: S103 CLK2), 1 = stamped by the update thread with its own frame
  bool moved;         // a reader outside the walks exists and stamps this input
  const char* board;  // the INPUTS_BOARD.md rows it covers
  const char* owner;  // S103 A3: who builds the reader while moved = false (the census prints "blocks the frame: name (owner)" for every open bit: the required mask says what blocks which frame)
};
inline constexpr Info kInputs[kInputCount] = {

    {kInPostSettings, "post_settings", 2, true, "P1-P9", "A"},
    {kInModelRuntime, "model_runtime", 2, true, "W3", "A"},
    {kInSphereOcc, "sphere_occ", 1, true, "W6", "lane 242"},
    {kInWrestlerPalette, "wrestler_palette", 2, true, "W1", "A2"},   // S103 A2: the latch reads [skeleton+200] (inputs_contrib.h LatchPalettes)
    {kInLightStates, "light_states", 2, rdr_scene::live::lightstamp::kStampLightStates, "W2, L1-L4", "A3 / A4 (stamp held: light_stamp_flag.h)"},
    {kInArenaWorlds, "arena_worlds", 2, true, "A1-A3", "A3 / C2"},   // S103 A3: stamped by the latch when the stage is known, its slot rows read clean and the clock origin known (inputs_contrib.h)
    {kInRingRows, "ring_rows", 2, rdr_scene::live::ringrecompute::kStampRingRows, "R1", "C3-C7 (released S103: run 27 clean + ring_compare)"},
    {kInCasterEntries, "caster_entries", 2, rdr_scene::live::caster_stamp::kStampCasterEntries, "S1-S5", "E3 (casters recomputed)"},
    {kInCirclePoints, "circle_points", 2, true, "S6", "A3"},   // S103 A3: points + ShadowParam + ViewProj at the latch (circle_points.h)
    {kInPlaneConstants, "plane_constants", 2, true, "S7-S9", "A3"},
    {kInCrowdSnapshot, "crowd_snapshot", 2, true, "C1", "D3"},   // S103 D3b: stamped AT THE LATCH by the crowd contributor for a snapshot the latch proved and that is whole (crowd_snapshot_core.h, crowd_latch_reader.h)
    {kInParticles, "particles", 2, true, "F1", "F (stamped per frame by presence, presence_rules.h; kStampParticles stays false)"},
    {kInCamera, "camera", 2, true, "K1", "A"},
    {kInMaterialParams, "material_params", 2, true, "W4", "A2 / A3"},   // wrestler skeletons' override tables (S103 A3); pieces are not covered (INPUTS_A2.md 4)
    {kInStageScreens, "screens", 2, true, "S10", "M"},   // S103 M: stamped at the latch only when every shown screen has its row (a screen the look shows but we cannot feed keeps the frame the game's)
    {kInStagePools, "stage_pools", 2, true, "B (L2)", "L2"},   // S103 L2: the decision side of the entrance page's inputs (STAGE_LIGHTING.md class B)
    {kInArenaModels, "arena_models", 2, true, "A2, A4", "A3"},
    {kInStageClock, "stage_clock", 2, rdr_scene::live::stageclock::kStampStageClock, "c186", "A3 / C2 (c186: source unknown, J1)"},
};
// S103 L2: a live writer of the pool / spot rows EXISTS (ReadStagePools below, decoded from the run 14 probe): an ENTRANCE frame stamps kInStagePools only when the latch read the rows (inputs_contrib.h), else the decision walks;
// match frames stamp. (It was false until the decode: every entrance frame walked.)
inline constexpr bool kStagePoolRowsExist = true;

// Per-input counters (relaxed; the census line reads them): stamps made, and the last frame stamped.
struct Counters {
  std::atomic<uint64_t> stamps[kInputCount] = {};
  std::atomic<uint64_t> last[kInputCount] = {};
};
inline Counters g_counters;

// ONE frame clock (lane A review 7): lane H's hooks decide with the HOOK clock (wwe2k14::g_guest_frame) and record j carries hook frame j - 1 (caster_contrib.h kHookToRecord; FRAME_CONTRACT 3). Every stamp is in the
// hook clock: a latch read of record j stamps j - 1, an update hook stamps its own hook frame. A kScInputStamps row marks its clock in `pad` (1 = hook clock).
inline constexpr uint64_t kHookToRecord = 1;
inline uint64_t HookFrameOfRecord(uint64_t record_frame) { return record_frame >= kHookToRecord ? record_frame - kHookToRecord : 0; }
inline constexpr uint8_t kClockHook = 1;

// Registers an input with the scene decision and its decision lag (S103 CLK2: scene_frame::g_input_lag, a per-bit store). Idempotent, safe to repeat.
inline void RequireBit(int bit) {
  for (const Info& i : kInputs)
    if (i.bit == bit) scene_frame::RequireInputLag(i.bit, i.lag);
}
// Registers every input that exists in every scene. NOT the sphere occluders: that input exists only where the caster switches register the sphere hook (wwe2k14_app.h calls RequireBit(kInSphereOcc) with
// RegisterSpheres), so Ours can happen without them (lane A review 7).
// S103 L2d (scene_kind.h): the per-SCENE required masks. Match and entrance need every registered input (today's mask; scene_kind refuses a drop for them). CAW drops the parts the editor has no producer for.
// S103 CW1 AUDIT (the editor = game sequence 76; each row: the game's own producer and whether it ever runs there; coverage = out\harness\coverage\coverage.json "caw" windows 003:caw.{loading,first60,templates,editor}):
//   crowd_snapshot (10)  DROP [C]: the stamp needs a snapshot the latch proved from the crowd manager's update (sub_8250BAF0 -> sub_82509CB8 palette copy, draw sub_82518950 / sub_82519508, singles sub_8276E7B0): none of
//                        them ran in caw (they ran in entrances / in_ring / pause_menu / replay / replay_play / story_scene only), so the editor never feeds it.
//   ring_rows (6)        DROP [C]: ring_recompute_contrib.h Take stamps only when the ring colour callback sub_825D0438 (-> sub_825D0340 / sub_825CE250) ran for the record (NotedFrame == frame); it did not run in caw,
//                        so there is no stamp (the `no_call` path returns before any row).
//   plane_constants (9)  DROP [C]: the stamp needs the plane node's own proxy copy for the hook frame (plane_proxy.h: the copy is taken at sub_827EF070's entry); sub_827EF070 / sub_827EE380 / sub_827EEF80 did not run in
//                        caw (claims_Q78 Q8), they ran in story / match_load / entrances / in_ring ...: no copy, no stamp.
//   stage_screens (14)   KEEP REQUIRED [C, S103 CAE]: the editor HAS screens, through the WRONG arena: the stream-free build published arena 0 (G+64, stale: the match settings' arena, never set in the editor)
//                        while the game builds the editor's own stage BG66 (sub_824E55B0 stores the id at stage+1216; CStage::MotionSetup ran in caw). Arena 0's text publishes screen movies for 800..809, `shown_` said shown, the
//                        editor has no screen source, so `complete` was false and the bit was never stamped (log 011: screens_complete 4907, incomplete 2848). The fix is the arena identity (arena_id_core.h ResolveArenaId takes
//                        the id from the stage object, G+64 only while no stage is known), not a drop; with BG66's own table there is no required screen. The `[host] ... never_fed` census names a live miss.
//   stage_pools (15)     KEEP REQUIRED [C, S103 CAE]: inputs_contrib.h stamps it on every frame that is not an entrance (`is_entrance = phase_readable && phase != 1`) and only an entrance needs rows. The scene phase word
//                        0x836F3940 is 4 in the editor [C: written at 0x83397CA0], not 1: so the editor reads as an entrance with no entering slot and the bit is NOT stamped today (log 011: it stops at editor entry). That is the
//                        open phase / look question (adversary site 3), settled by a probe first; it is not changed here. The crowd is stamped empty in the editor (crowd_snapshot stays dropped above).
// Everything else stays required, fail closed: arena_worlds / arena_models / stage_clock (the editor HAS a stage: sub_824E5340 CStage::MotionSetup ran in caw), caster_entries for the shadow + rim (shadow tick sub_827E95D8
// and rim sub_827EA098 ran in caw), camera, model_runtime, wrestler_palette (model submit sub_82C13C88 / colour pass sub_82C04DF0 ran in caw). Coverage is a union per window: "never ran" = not seen in those windows [C for them].
inline constexpr uint32_t kCawDrop = (1u << kInCrowdSnapshot) | (1u << kInRingRows) | (1u << kInPlaneConstants);
static_assert((kCawDrop & ((1u << kInCamera) | (1u << kInCasterEntries) | (1u << kInArenaWorlds) | (1u << kInArenaModels) | (1u << kInModelRuntime) | (1u << kInWrestlerPalette) | (1u << kInStageScreens) | (1u << kInStagePools))) == 0,
              "kCawDrop must never drop the camera, the caster entries, the arena worlds / models, the model runtime, the wrestler palette or the two inputs the editor feeds (screens, pools) (L2dF review, CW1 audit)");
inline void RegisterSceneMasks() { scene_kind::SetKindDrop(scene_kind::Kind::Caw, kCawDrop); }
// The required mask of a scene kind over every input (the unit's view; the decision applies the same drop to what is registered).
inline uint32_t SceneMask(scene_kind::Kind k) { return scene_kind::RequiredFor(k, (1u << kInputCount) - 1u); }
inline void RegisterAll() {
  for (const Info& i : kInputs)
    if (i.bit != kInSphereOcc) scene_frame::RequireInputLag(i.bit, i.lag);
  RegisterSceneMasks();
}

// S103 L1C: the run-time hold (--wwe2k14_rdr_scene_hold=<bit list>, hold_switch.h): a held bit is never stamped, so a frame needing it stays the game's, exactly as when the bit was a constexpr OFF. Default 0 = nothing
// held. `held_drops` counts the stamps a hold swallowed (the census prints it) so a bisect can see the hold working.
inline std::atomic<uint32_t> g_hold_mask{0};
inline std::atomic<uint64_t> g_held_drops[kInputCount] = {};
inline bool Held(int bit) { return bit >= 0 && bit < 32 && ((g_hold_mask.load(std::memory_order_relaxed) >> bit) & 1u) != 0; }

// The one stamp: this input holds the value of `frame`. Where the game computes it, or at the latch.
inline void Capture(int bit, uint64_t frame) {
  if (bit < 0 || bit >= kInputCount) return;
  if (Held(bit)) {
    g_held_drops[bit].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  rdr_own::InputCaptured(rdr_own::Pass::Scene3D, bit, frame);
  g_counters.stamps[bit].fetch_add(1, std::memory_order_relaxed);
  g_counters.last[bit].store(frame, std::memory_order_relaxed);
}

// Mask of the inputs that have a reader outside the walks (the others are the open INPUTS_BOARD.md rows).
inline uint32_t MovedMask() {
  uint32_t m = 0;
  for (const Info& i : kInputs)
    if (i.moved) m |= 1u << i.bit;
  return m;
}
inline uint32_t AllMask() { return (1u << kInputCount) - 1u; }

// ---- reading the guest, pure: `read(guest, dst, n)` copies n raw (big-endian) guest bytes, false when unreadable.
using ReadFn = bool (*)(uint32_t guest, void* dst, size_t n);
inline uint32_t Swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
inline bool ReadWord(ReadFn read, uint32_t guest, uint32_t& v) {
  uint32_t raw = 0;
  if (!read(guest, &raw, 4)) return false;
  v = Swap32(raw);
  return true;
}

// S103 FD (CF2, docs\research\s103\CF_crowd_fade.md): the stage-page pass object's own clock. The object is a task of task group 11 (docs\systems\task_runner.md: owner [0x83930050]; [owner+32] = POINTER to the
// group array, [owner+40] = its end, group g at [owner+32] + 16g: +0 = first task word, +8 = end; sub_82C2FEB8 / sub_82C302A0; S103 FDP: the first version read the array inline at owner+32 and never found it) whose
// vtable slot 1 (word at vtable+4) is sub_825BA448 [C listing 0x825BA448-0x825BA46C: if [obj+112] != 0 then [obj+108] += the 60/fps tick]; created by
// sub_825BB398 from sub_824EDB50 (0x824EDC68) at match load, which registers obj+32 in the stage's list at stage+1184 (no stage field holds obj itself: the task list is the stable path, the same walk
// as scene_freeze.h RunnerEnter: src\task_group_walk.h, one copy). The entrance word is [obj+88] (sub_825BB0E0 0x825BB21C: [this+72], this = obj+16). Row kSlPageClock: dir[0] = t, flags bit 0 = running, bit 1 = entrance word != 0.
// `cache` = the object found last time (revalidated every latch: [[obj]+4] == sub_825BA448); a miss rescans every kPageClockRescan latches. false = no object (menus, loading): no row.
// `info` (optional) = what the last rescan saw of the group array: [owner+32], the group count (35 expected), group 11's task count (the census line page_clock [...]).
inline constexpr uint32_t kTaskOwnerGlobal = 0x83930050u, kPageClockGroup = 11, kPageUpdateFn = 0x825BA448u, kPageEntranceOff = 88, kPageClockOff = 108, kPageRunOff = 112, kPageClockRescan = 8;
inline bool ReadStagePageClock(ReadFn read, uint32_t& cache, uint32_t& misses, rdr_scene::StageLookRow& out, task_walk::GroupInfo* info = nullptr) {
  const auto valid = [&](uint32_t obj) {
    uint32_t vt = 0, fn = 0;
    return obj >= 0x40000000u && obj < 0xE0000000u && ReadWord(read, obj, vt) && ((vt >> 24) == 0x82u || (vt >> 24) == 0x83u) && ReadWord(read, vt + 4, fn) && fn == kPageUpdateFn;
  };
  if (!read) return false;
  if (cache && !valid(cache)) cache = 0;
  if (!cache) {
    if (misses++ % kPageClockRescan != 0) return false;
    uint32_t owner = 0;
    if (!ReadWord(read, kTaskOwnerGlobal, owner) || owner < 0x40000000u || owner >= 0xE0000000u) return false;
    const auto rd = [&](uint32_t a, uint32_t& v) { return ReadWord(read, a, v); };
    task_walk::GroupInfo gi;
    const bool ok = task_walk::ReadGroup(rd, owner, kPageClockGroup, gi);
    if (info) *info = gi;
    if (!ok) return false;
    task_walk::ForEachGroupTask(rd, owner, kPageClockGroup, [&](uint32_t t, uint32_t) {
      if (valid(t)) cache = t;
      return cache != 0;
    });
    if (!cache) return false;
    misses = 0;
  }
  uint32_t ent = 0, run = 0, tbits = 0;
  if (!ReadWord(read, cache + kPageEntranceOff, ent) || !ReadWord(read, cache + kPageClockOff, tbits) || !ReadWord(read, cache + kPageRunOff, run)) return false;
  float t;
  std::memcpy(&t, &tbits, 4);
  out = rdr_scene::StageLookRow();
  out.kind = rdr_scene::kSlPageClock;
  out.dir[0] = t;
  out.flags = (run != 0 ? 1u : 0u) | (ent != 0 ? 2u : 0u);
  return true;
}
// kScPostParams: S's first kPostSettingsWords words, host order. `row.flags` carries kPpRead only when the whole block was read.
inline rdr_scene::PostParamsRow ReadPostSettings(ReadFn read, uint64_t frame) {
  using namespace rdr_scene;
  PostParamsRow row;
  row.frame = frame;
  uint32_t s = 0;
  bool ok = read && ReadWord(read, post_settings::kGuestPointer, s) && s != 0;
  row.settings_addr = ok ? s : 0;
  if (ok) {
    uint32_t raw[kPostSettingsWords];
    ok = read(s, raw, sizeof raw);
    if (ok)
      for (uint32_t i = 0; i < kPostSettingsWords; ++i) row.s[i] = Swap32(raw[i]);
  }
  row.flags = post_settings::FlagsOf(row, ok);
  {   // S103 FXF (review 6): the game's display size, the unit of the afterimage's centre (S+232 / +236): [[0x83929A68]+300] x +304; 0 = unreadable (the reader falls back to 1280 x 720 and counts it)
    uint32_t disp = 0, dw = 0, dh = 0;
    if (read && ReadWord(read, 0x83929A68, disp) && disp && ReadWord(read, disp + 300, dw) && ReadWord(read, disp + 304, dh) && dw >= 64 && dw <= 16384 && dh >= 64 && dh <= 16384)
      row.fx[kFxDispW] = dw, row.fx[kFxDispH] = dh;
  }
  {   // S103 FXH: the game's frame rate [0x8392DC58] (a float, big-endian word -> host bits): the retro film's clock steps by 1 / rate per game frame (sub_82C288B0). 0 = unreadable (the reader uses 30)
    uint32_t bits = 0;
    if (read && ReadWord(read, 0x8392DC58, bits)) row.fx[kFxRate] = bits;
  }
  return row;
}

// kScModelRuntime: MdlAlpha = the float at [model+252] (INPUTS_BOARD.md W3). A model it cannot read gets no row (counted by the caller).
inline constexpr uint32_t kModelAlphaOff = 252;
// `pass` (S103 WRK, review F5): bit 0 = read the char pass mode (+616 / +620) -- a wrestler model with the switch on; bit 1 = also latch the part table's E+20 words. 0 = MdlAlpha only (an arena model, or the switch off:
// the row is then strictly what it was before the lane).
inline constexpr unsigned kReadPassMode = 1, kReadPartE20 = 2;
inline bool ReadModelRuntime(ReadFn read, uint32_t model, uint64_t frame, rdr_scene::ModelRuntimeRow& out, unsigned pass = 0) {
  uint32_t w = 0;
  if (!read || !ReadWord(read, model + kModelAlphaOff, w)) return false;
  out = rdr_scene::ModelRuntimeRow{};
  out.object = model;
  out.frame = frame;
  out.flags = rdr_scene::kMrAlphaRead;
  float a = 0;
  std::memcpy(&a, &w, 4);
  if (!wwe2k14::safe_pages::ModelAlphaPlausible(a)) return false;   // BH3-4: non-finite = a stale or reused block: no row
  out.alpha = a;
  // WRK (WRK.md 2d [C]): the char pass mode the game stores per character draw: byte 0 = [model+616] (0 / 2), byte 1 = [model+620]. An unreadable / insane word = no flag (the row keeps MdlAlpha).
  uint32_t m616 = 0, m620 = 0;
  if ((pass & kReadPassMode) && ReadWord(read, model + 616, m616) && ReadWord(read, model + 620, m620) && m616 <= 3 && m620 <= 3) {
    out.reserved = m616 | (m620 << 8);
    out.flags |= rdr_scene::kMrPassModeRead;
    // WRK_ADV 5b [C]: sub_82BD5EE0 adds +16 to a part's pass when [[model+480] + 24 * part + 20] != 0 and model+620 == 1. Latched for the census only (the pick on +620 stays off until a milestone run settles
    // the hair conflict): byte 2 = any part's E+20 word is non-zero, byte 3 = the part table was read (0 = unknown). The table is one block of 24 bytes per part, parts = [model+372].
    uint32_t tab = 0, parts = 0;
    if ((pass & kReadPartE20) && ReadWord(read, model + 480, tab) && ReadWord(read, model + 372, parts) && tab >= 0x10000 && parts && parts <= 64) {
      uint32_t blk[64 * 6];
      if (read(tab, blk, parts * 24)) {
        uint32_t any = 0;
        for (uint32_t p = 0; p < parts; ++p) any |= blk[p * 6 + 5];
        out.reserved |= ((any ? 1u : 0u) << 16) | (1u << 24);
      }
    }
  }
  return true;
}


// ---- W1 (S103 A2): the skin palette AS THE DRAW READS IT = count x 64 bytes at the pointer [skeleton+200] (the consumer copy sub_82C04DF0 took inside the walk; WRESTLER_PROMOTION_MAP.md). Read at the latch.
// The source is update-side state (poses sub_82BD30E8 -> skin sub_82BD35B0 -> delta sub_82BD3620, INPUTS_BOARD.md W1); that the nodes 59-108 writer is also update-side is [U] until the mirror run's
// census compares this read with the walk's consumer copy (the walk still runs in mirror mode). The bound is a sanity limit, not a rule: a wrestler skeleton has ~109 nodes (assets.md).
inline constexpr uint32_t kPaletteOff = 200, kPaletteMaxNodes = 256;
// out = count Mat4 (host floats, memory order = the consumer's); false when the pointer is null / the block unreadable / a value not finite (a NaN palette is never recorded).
inline bool ReadPalette(ReadFn read, uint32_t skel, uint32_t count, std::vector<rdr_scene::Mat4>& out, std::vector<uint32_t>& raw) {   // `raw` = the caller's scratch (no allocation once grown)
  out.clear();
  if (!read || count == 0 || count > kPaletteMaxNodes) return false;
  uint32_t ptr = 0;
  if (!ReadWord(read, skel + kPaletteOff, ptr) || ptr == 0) return false;
  raw.resize(size_t(count) * 16);
  if (!read(ptr, raw.data(), raw.size() * 4)) return false;
  out.resize(count);
  for (size_t i = 0; i < raw.size(); ++i) {
    const uint32_t w = Swap32(raw[i]);
    float f;
    std::memcpy(&f, &w, 4);
    if (!std::isfinite(f)) return out.clear(), false;
    out[i / 16].m[i % 16] = f;
  }
  return true;
}

// ---- W2 / L1-L4 (S103 A2): the light objects and the manager's selection words, read at the latch (docs\renderer\INPUTS_A2.md).
// Sources [C listings read with tools\analysis\list_function.py on generated\default: sub_827F6828 (select), sub_827F6910 / sub_826203E8 (install), PER_DRAW_LIGHTS.md 1.1 for what writes the objects]:
//   mgr = word 0x83924778 (sub_826203E8 0x82620448-0x82620450: lis -31854 / lwz 18296);  n = mgr+124;  sets = pointer array [mgr+112];  ids = [mgr+116];  remap = int32 array [mgr+120] (n entries; -1 = none);
//   pool = pointer array [mgr+140], count mgr+144 (sub_827F6828 0x827F68B8-0x827F68C8: pool[(remap)[index]]);  override object = POINTER at mgr+368 (0x827F6874 `lwz r3,368(r31)` returns it), used for id 10010 when
//   mgr+372 != 0 (0x827F6850-0x827F6874);  blend object = POINTER at mgr+376 [C, the Opus review of A2: same shape as +368].
//   Mode override (sub_826203E8 0x82620410-0x8262043C): mode 0 -> word 0x83622668, mode 1 -> word 0x8362266C, taken when >= 0 and not skipped by the gate (word 0x836F53D0 non-null with [+152] != 0).
// The block a node installs is object+48, 896 bytes (sub_8261FE90 = sub_82BCC018(G, obj+48), PER_DRAW_LIGHTS.md 1.1). Null set / pool slots are NORMAL (sub_827F8808 skips them): counted apart, never an error.
// **Why this lock-free read is safe [C PER_DRAW_LIGHTS.md 1.1 / the review]:** the manager's own critical section is at mgr+64 (sub_827F6828 / sub_827F6910 enter it: `addi r30,r3,64 ... bl 0x83596fb4`);
// the latch takes no lock, so the read is consistent ONLY while every barrier client (the update tasks that write the objects, the draw threads that install) is parked, which is the latch's own precondition
// (tail_latch.h, S102 U1). A latch that ever runs with a writer live must take that section first.
inline constexpr uint32_t kLightMgrPtr = 0x83924778, kLightOverrideWord0 = 0x83622668, kLightOverrideWord1 = 0x8362266C, kLightGatePtr = 0x836F53D0;
inline constexpr uint32_t kLightBlockOff = 48;
inline constexpr uint32_t kTrackBlockOff = 148, kTrackBlockWords = 50;   // S103 TNB: mgr + 148 .. 347 (200 bytes: 10 binding ints, 20 size ints, 10 u64 keys)
inline bool ReadLightBlockAt(ReadFn read, uint32_t block, uint32_t* words) {   // the 896 bytes starting AT `block`
  if (!read || block < 0x10000) return false;
  uint32_t raw[rdr_scene::kLsLightFloats];
  if (!read(block, raw, sizeof raw)) return false;
  for (uint32_t i = 0; i < rdr_scene::kLsLightFloats; ++i) words[i] = Swap32(raw[i]);
  return true;
}
inline bool ReadLightBlock(ReadFn read, uint32_t obj, uint32_t* words) {
  if (!read || obj < 0x10000) return false;
  uint32_t raw[rdr_scene::kLsLightFloats];
  if (!read(obj + kLightBlockOff, raw, sizeof raw)) return false;
  for (uint32_t i = 0; i < rdr_scene::kLsLightFloats; ++i) words[i] = Swap32(raw[i]);
  return true;
}
enum class LightWhy : uint32_t { Ok = 0, ManagerUnreadable = 1, NoSets = 2, Insane = 3 };
struct LightReadInfo {
  LightWhy why = LightWhy::Ok;
  uint32_t unreadable = 0;   // non-null objects whose block could not be read
  uint32_t null_objects = 0; // null set / pool slots (normal)
  uint32_t sets = 0, pool = 0, distinct = 0;
  std::vector<uint32_t> objects;   // EVERY non-null set / pool / blend / override object address (a deduped block keeps only the first object's address in its row: review must 5)
};
// false = no rows (info.why says which: the manager unreadable, n == 0 = no sets yet, or not sane).
inline bool ReadLightObjects(ReadFn read, uint64_t frame, std::vector<rdr_scene::LightObjectRow>& out, LightReadInfo& info) {
  using namespace rdr_scene;
  info = LightReadInfo{};
  out.clear();
  uint32_t mgr = 0, n = 0, sets = 0, ids = 0, remap = 0, pool = 0, pool_n = 0, ovr = 0, ovr_flag = 0, blend = 0;
  if (!read || !ReadWord(read, kLightMgrPtr, mgr) || mgr < 0x10000) return info.why = LightWhy::ManagerUnreadable, false;
  if (!ReadWord(read, mgr + 124, n) || !ReadWord(read, mgr + 112, sets) || !ReadWord(read, mgr + 116, ids) || !ReadWord(read, mgr + 120, remap) || !ReadWord(read, mgr + 140, pool) ||
      !ReadWord(read, mgr + 144, pool_n) || !ReadWord(read, mgr + 368, ovr) || !ReadWord(read, mgr + 372, ovr_flag) || !ReadWord(read, mgr + 376, blend))
    return info.why = LightWhy::ManagerUnreadable, false;
  if (n == 0) return info.why = LightWhy::NoSets, false;
  if (n > kLightMaxSets || pool_n > kLightMaxPool || !sets || !ids || !remap || (pool_n && !pool)) return info.why = LightWhy::Insane, false;
  uint32_t set_ptr[kLightMaxSets], id_w[kLightMaxSets], remap_w[kLightMaxSets], pool_ptr[kLightMaxPool];
  if (!read(sets, set_ptr, 4 * n) || !read(ids, id_w, 4 * n) || !read(remap, remap_w, 4 * n) || (pool_n && !read(pool, pool_ptr, 4 * pool_n))) return info.why = LightWhy::ManagerUnreadable, false;
  // the manager row first (row 0); the block rows follow
  out.emplace_back();
  {
    LightObjectRow& m = out[0];
    m.frame = frame, m.kind = kLoManager, m.remap = int32_t(ovr_flag), m.aux0 = n, m.aux1 = pool_n;
    uint32_t w0 = 0xFFFFFFFFu, w1 = 0xFFFFFFFFu, gate = 0, gate_flag = 0;   // unread override words must not read as index 0
    bool all = ReadWord(read, kLightOverrideWord0, w0);
    all = ReadWord(read, kLightOverrideWord1, w1) && all;
    if (!ReadWord(read, kLightGatePtr, gate)) all = false;
    else if (gate >= 0x10000 && !ReadWord(read, gate + 152, gate_flag)) all = false;
    m.words[kLmOverrideMode0] = w0, m.words[kLmOverrideMode1] = w1, m.words[kLmGateObject] = gate, m.words[kLmGateFlag] = gate_flag;
    for (uint32_t k = 0; k < kLmRefCount; ++k) m.words[kLmRefBase + k] = kLightNoRow;
    if (all) m.flags = kLoBlockRead;
  }
  // Identical blocks share one row: the 896-byte compare of an earlier row of this frame (at most ~100 objects; the arena sets are mostly one black set).
  auto ref_of = [&](uint32_t kind, uint32_t index, uint32_t obj) -> uint32_t {
    if (!obj) return ++info.null_objects, kLightNoRow;
    info.objects.push_back(obj);
    LightObjectRow r;
    r.frame = frame, r.kind = kind, r.index = index, r.object = obj;
    if (!ReadLightBlock(read, obj, r.words)) return ++info.unreadable, kLightNoRow;
    r.flags = kLoBlockRead;
    for (uint32_t i = 1; i < out.size(); ++i)
      if (std::memcmp(out[i].words, r.words, sizeof r.words) == 0) return i;
    out.push_back(r);
    ++info.distinct;
    return uint32_t(out.size() - 1);
  };
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t row = ref_of(kLoSet, i, Swap32(set_ptr[i]));
    LightObjectRow& m = out[0];   // (out may have grown; index again)
    m.words[kLmRefBase + i] = row;
    m.words[kLmSetInfoBase + i] = (Swap32(id_w[i]) & 0xFFFFu) | (uint32_t(uint16_t(int32_t(Swap32(remap_w[i])) + 1)) << 16);
    ++info.sets;
  }
  for (uint32_t j = 0; j < pool_n; ++j) {
    const uint32_t row = ref_of(kLoPool, j, Swap32(pool_ptr[j]));
    out[0].words[kLmRefBase + kLightMaxSets + j] = row;
    ++info.pool;
  }
  { const uint32_t row = ref_of(kLoBlend, 0, blend); out[0].words[kLmRefBase + rdr_scene::kLmRefBlend] = row; }
  { const uint32_t row = ref_of(kLoOverride, 0, ovr); out[0].words[kLmRefBase + rdr_scene::kLmRefOverride] = row; }
  {   // S103 TNB (TEN.md 4): the entrance page's per-track words, ONE 200-byte block at mgr + 148: 10 binding ints, 2 x 10 size ints, 10 u64 keys (sub_827F65C8 / sub_827F65D8). Unread = no flag (the words stay 0 and mean nothing).
    uint32_t raw[kTrackBlockWords];
    if (read(mgr + kTrackBlockOff, raw, sizeof raw)) {
      LightObjectRow& m = out[0];
      for (uint32_t i = 0; i < kLightTracks; ++i) m.words[kLmTrackBind + i] = Swap32(raw[i]);
      for (uint32_t i = 0; i < 2 * kLightTracks; ++i) m.words[kLmTrackSize + i] = Swap32(raw[kLightTracks + i]);
      for (uint32_t i = 0; i < 2 * kLightTracks; ++i) m.words[kLmTrackKey + i] = Swap32(raw[3 * kLightTracks + i]);   // u64 i = (high, low) in guest order = word 2 i, 2 i + 1
      m.flags |= rdr_scene::kLoTracksRead;
    }
  }
  return true;
}
// S103 TNB: the kSlTrackBind rows of a frame: for every track whose binding word is a slot 0..8 (the game's own range test, sub_825BACE0 0x825BAEAC) and whose character resolves (the table 0x836F38D0 entry non-null, [char + 176]
// non-null), the 16 bytes at [char + 176] (x, y, z, w: the skeleton's node-2 translation, which the game writes over the track's row at 0x825BAECC). `act25000` = the character's act (the u16 of the act record's first word) is the
// entrance body act: the writer of track 0 is then sub_8231FCC0's, else YAT2 code 208's (census only). A track whose character does not resolve gets no row (the game keeps the rig's row there too). Pure guest reads.
inline constexpr uint32_t kCharSlotTable = 0x836F38D0, kEntranceAct = 25000;
struct TrackBindInfo { uint32_t bound = 0, unresolved = 0, nonfinite = 0; };
inline void ReadTrackBinds(ReadFn read, const rdr_scene::LightObjectRow& mgr, std::vector<rdr_scene::StageLookRow>& out, TrackBindInfo& info) {
  using namespace rdr_scene;
  if (!read || mgr.kind != kLoManager || !(mgr.flags & kLoTracksRead)) return;
  for (uint32_t i = 0; i < kLightTracks; ++i) {
    const int32_t b = int32_t(mgr.words[kLmTrackBind + i]);
    if (b < 0 || b >= 9) continue;
    ++info.bound;
    uint32_t ch = 0, p = 0, raw[4];
    if (!ReadWord(read, kCharSlotTable + 4 * uint32_t(b), ch) || ch < 0x10000 || !ReadWord(read, ch + 176, p) || p < 0x10000 || !read(p, raw, sizeof raw)) { ++info.unresolved; continue; }
    float v[3];
    for (int c = 0; c < 3; ++c) { const uint32_t w = Swap32(raw[c]); std::memcpy(&v[c], &w, 4); }
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) { ++info.nonfinite; continue; }
    uint32_t act = 0, word = 0;
    const bool act25000 = ReadWord(read, ch + 28, act) && act >= 0x10000 && ReadWord(read, act, word) && (word >> 16) == kEntranceAct;
    StageLookRow r;
    r.kind = kSlTrackBind, r.rig_slot = uint16_t(i + 1), r.flags = uint32_t(b + 1) | (act25000 ? 0x100u : 0u), r.dir[0] = v[0], r.dir[1] = v[2], r.dir[2] = v[1];
    out.push_back(r);
  }
}

// ---- W4 / A4 (S103 A2): a model's run-time parameter override entries (see scene_record.h kScParamOverrides for the listing facts).
inline constexpr uint32_t kModelBlockOff = 832, kModelPartsOff = 372;   // B = [model+832]; the part count [model+372] (0x82C05908)
inline constexpr uint32_t kBlkWideTable = 1132, kBlkWideCount = 1136, kBlkPartTables = 1140, kBlkPartCounts = 1144;
inline constexpr uint32_t kOverrideEntryBytes = 20, kOverrideMaxEntries = 64, kOverrideMaxParts = 64, kOverrideMaxRows = 1024, kArenaOverrideMaxRows = 4096;   // rows: wrestlers 1024 in all, the arena's models 4096 more (separate budgets, review F4)   // total over every model of the frame (S103 A3: the arena's ~200 models join the wrestlers)
struct ParamReadInfo {
  uint32_t entries = 0, value_unreadable = 0, name_unreadable = 0;
  bool overflow = false;   // the row budget ran out (counted apart from an unreadable table)
  bool no_block = false;   // [model+832] null: nothing to override (not a failure)
};
// false = a table could not be read or is not sane (the model gets no stamp); rows read so far for this model are dropped.
inline bool ReadParamOverrides(ReadFn read, uint32_t model, uint64_t frame, std::vector<rdr_scene::ParamOverrideRow>& out, ParamReadInfo& info, size_t row_limit = kOverrideMaxRows) {
  using namespace rdr_scene;
  const size_t keep = out.size();
  auto fail = [&]() { out.resize(keep); return false; };
  uint32_t b = 0, parts = 0;
  if (!ReadWord(read, model + kModelBlockOff, b)) return fail();
  if (b < 0x10000) return info.no_block = true, true;
  if (!ReadWord(read, model + kModelPartsOff, parts) || parts > kOverrideMaxParts) return fail();
  auto entries = [&](uint32_t ptr, uint32_t n, uint32_t part) -> bool {
    if (!n) return true;
    if (out.size() + n > row_limit) return info.overflow = true, false;
    if (n > kOverrideMaxEntries || ptr < 0x10000) return false;
    uint32_t raw[kOverrideMaxEntries * 5];
    if (!read(ptr, raw, n * kOverrideEntryBytes)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      ParamOverrideRow r;
      r.frame = frame, r.model = model, r.part = part, r.index = i;
      for (int k = 0; k < 5; ++k) r.entry[k] = Swap32(raw[i * 5 + k]);
      uint32_t nm[4];
      if (r.entry[2] >= 0x10000 && read(r.entry[2], nm, sizeof nm)) {   // the name: a const char* (16 bytes read, 15 kept + NUL)
        std::memcpy(r.name, nm, 15);
        r.name[15] = 0;
        r.flags |= kPoNameRead;
      } else {
        ++info.name_unreadable;
      }
      // WRK F1 (WRK.md 1b.4 / 1d.5, WRK_ADV 1c [C]): the LIVE value is the SOURCE pointer entry[1] (a relinked controller record's value at +20, else the file value); entry[0] is the 360's per-frame
      // double buffer (slot 0 is stale). The size is entry[3] = the game's own memcpy size (entry+12: 4 for descriptor value types 1 / 2, else 4 * entry[4]), at most 64 bytes: a controller record is 28 bytes, a fixed
      // 64-byte read ran past it, and 4 * entry[4] is wrong for the value-type 1 / 2 names. At least one word.
      const uint32_t nw = std::clamp<uint32_t>((r.entry[3] + 3) / 4, 1u, 16u);
      uint32_t v[16] = {};
      if (r.entry[1] >= 0x10000 && read(r.entry[1], v, nw * 4)) {
        for (uint32_t k = 0; k < nw; ++k) r.value[k] = Swap32(v[k]);
        r.flags |= kPoValueRead;
      } else {
        ++info.value_unreadable;
      }
      out.push_back(r);
      ++info.entries;
    }
    return true;
  };
  uint32_t wide = 0, wide_n = 0, tables = 0, counts = 0;
  if (!ReadWord(read, b + kBlkWideTable, wide) || !ReadWord(read, b + kBlkWideCount, wide_n) || !ReadWord(read, b + kBlkPartTables, tables) || !ReadWord(read, b + kBlkPartCounts, counts)) return fail();
  if (!entries(wide, wide_n, kParamModelWide)) return fail();
  if (parts && ((tables >= 0x10000) != (counts >= 0x10000))) return fail();   // a half-null part table (one of the two arrays missing) is a failure, never a partial pass
  if (tables >= 0x10000 && counts >= 0x10000) {
    for (uint32_t p = 0; p < parts; ++p) {
      uint32_t ptr = 0, n = 0;
      if (!ReadWord(read, tables + 4 * p, ptr) || !ReadWord(read, counts + 4 * p, n)) return fail();
      if (!entries(ptr, n, p)) return fail();
    }
  }
  return true;
}

// ---- S7 (S103 A2): the plane shadow module's per-frame constants, read at the latch (kScPlaneFrames; docs\renderer\INPUTS_A2.md 5).
// Sources [C]: the plane module singleton pointer is word 0x83924758 (shadows.md 1: `sub_827ECB60` tests it; destroyed by sub_822A75B8). The constants the walk's recorder uploads are the module's own fields (plane_sites.h, each row cited from a
// listing): fade rows module+68..+104 (10 floats; the weight is +104), the camera block module+1072 (16 floats), the screen fit module+1312 (16 floats). They are written by the update task sub_827EE380 (vtable data words 0x82033CBC /
// 0x821F6710; it calls sub_827EE2C0 at 0x827EE474, which copies the main camera into +1072 and calls sub_827ED960 to build the fit [C q callers]); so they are update-side state. plane_normal is the constant (0, -1, 0, 0) the walk builds in
// its own frame (plane_sites.h 0x827EC6DC). `have` is the full set: all four parts were read at the latch.
// NOT covered (S8): the per-draw rows (plane_pos per proxy kind: +1440 and the proxy list; the blur block = camera x fit transposed) need the proxy list, which only the walk enumerates [U].
inline constexpr uint32_t kPlaneModulePtr = 0x83924758, kPlFadeOff = 68, kPlCameraOff = 1072, kPlFitOff = 1312;
// The game refreshes these fields only when sub_827EE380's own tests pass and sub_827EE2C0's camera test holds [C listings 0x827EE380-0x827EE3A8, 0x827EE2D0-0x827EE2E0; the review of part 3]:
//   [module+40] != 0 [C, coordinator listing read: the constructor sub_827EE860 stores vtables 0x82033CC8 at +0, 0x82033CC0 at +4 and 0x82033CB8 at +20 (0x827EE8D0-0x827EE8DC); the data word 0x82033CBC that points at
//   sub_827EE380 is slot 1 of the +20 vtable, so the task is a method of the sub-object at module+20: its r3 = module+20 and `lwz r11,20(r3)` at 0x827EE38C reads module+40, zeroed by the constructor at 0x827EE8C8)
//   and word 0x836F39A0 == 0 (0x827EE3A0-0x827EE3A8),
//   [[0x83929A68] + 256] != 0 (0x827EE2D4-0x827EE2E0: the camera copy and the fit are skipped when it is null).
// A frame in which any of the three fails holds STALE fields: no PlaneFrame is read for it (each refusal counted apart).
inline constexpr uint32_t kPlEnabledOff = 40, kPlSuppressWord = 0x836F39A0, kPlCamHolder = 0x83929A68, kPlCamHolderOff = 256;
// S103 CE: the old `Unreadable` (index 5) is split into five reasons so the census says which (the entrances' 1,030 bit-9 misses): ReadFail = a guest read failed; HolderNull = the camera holder [0x83929A68] < 0x10000;
// NonFiniteFade / NonFiniteCamera / NonFiniteFit = a NaN / inf in that block. Log first: the stamp decision is unchanged (every reason but Ok leaves have_plane false).
enum class PlaneWhy : uint32_t { Ok = 0, NoModule, Disabled, Suppressed, NoCamera, ReadFail, HolderNull, NonFiniteFade, NonFiniteCamera, NonFiniteFit, OffScreenFit };   // S103 PB: OffScreenFit appended (index 10) so plane_bad_ indices 7-9 stay
inline constexpr uint32_t kPlaneWhyCount = 11;
// The first bad block of a non-finite reason (up to 16 floats) for the dev log; null = not wanted.
struct PlaneBadBlock { float v[16]; uint32_t n = 0; };
// S103 PLF: WHY the game built no plane module (docs\research\s103\SHD.md PLB claims 1-5). sub_827F6368 builds it (sub_827EE860) only if sub_827E3520() < 3 AND bytes G+19 and G+24 of G = 0x8376AAF4
// (sub_827D7578's constant return) are 0; G+18 is the sibling structure flag (sub_8256E880 "plain ring" = G+18, +19, +24 all 0). Pure guest reads, nothing called. Census text: `structure a/b/c, participants N`
// (a = G+18, b = G+19, c = G+24; tools\weapons\feed_census_check.py reads it: a byte != 0 or N >= 3 = the game's own "no module").
// The count follows sub_827E3520's slot loop [C listing 0x827E3550-0x827E358C]: slot i = sub_827D7368(i) = sub_827F4D28(P+144, i) = P+144+56*i+4 = P+148+56*i, P = [0x8376AAF0]; counted when the id word [slot+16]
// (sub_822FF368) != 51200 (0xC800 = empty) and [slot+8] is 0, 2 or 3 (0x827E3568-0x827E3584). The loop is the branch taken when [G+18576] == 3 and [0x839295C4] == 0 (0x827E3534, 0x827E3548); any other state takes
// the function's other branches (not read: `loop_path` false) and the count is then UNCONFIRMED.
inline constexpr uint32_t kPlGlobal = 0x8376AAF4u, kPlPartPtr = 0x8376AAF0u, kPlOffStructA = 18, kPlOffStructB = 19, kPlOffStructC = 24, kPlOffMode = 18576, kPlNetWord = 0x839295C4u;
inline constexpr uint32_t kPlSlotBase = 148, kPlSlotStride = 56, kPlSlots = 6, kPlSlotIdOff = 16, kPlSlotStatusOff = 8, kPlSlotEmptyId = 0xC800u;
// S103 MBC review 1: count_known = the slot-loop count is the whole answer of sub_827E3520. The loop runs on BOTH branches (the else branch at 0x827E3670); the only addition is the online add-on (sub_82B5AA48() == 1 and [0x83929894] != 0).
// So: mode 3 -> known when net == 0; any other mode -> known when [0x83929894] == 0. A failed read = unknown. `loop_path` stays the PLF census flag (its text is unchanged).
struct PlaneStructure { uint32_t a = 0, b = 0, c = 0, participants = 0; bool ok = false, loop_path = true, count_known = false; };
inline constexpr uint32_t kPlOnlineWord = 0x83929894u;
inline bool ReadGuestByte(ReadFn read, uint32_t guest, uint32_t& v) {   // the guest is big-endian: byte k of an aligned word is bits 24-8k
  uint32_t w = 0;
  if (!ReadWord(read, guest & ~3u, w)) return false;
  v = (w >> (24 - 8 * (guest & 3u))) & 0xFFu;
  return true;
}
inline bool ReadPlaneStructure(ReadFn read, PlaneStructure& out) {
  PlaneStructure r;
  uint32_t mode = 0, net = 0, part = 0;
  if (!read || !ReadGuestByte(read, kPlGlobal + kPlOffStructA, r.a) || !ReadGuestByte(read, kPlGlobal + kPlOffStructB, r.b) || !ReadGuestByte(read, kPlGlobal + kPlOffStructC, r.c)) return false;
  if (!ReadWord(read, kPlGlobal + kPlOffMode, mode) || !ReadWord(read, kPlNetWord, net) || !ReadWord(read, kPlPartPtr, part) || part < 0x10000) return false;
  r.loop_path = mode == 3 && net == 0;
  uint32_t online = 1;
  const bool online_read = ReadWord(read, kPlOnlineWord, online);
  r.count_known = mode == 3 ? net == 0 : (online_read && online == 0);
  for (uint32_t i = 0; i < kPlSlots; ++i) {
    const uint32_t slot = part + kPlSlotBase + kPlSlotStride * i;
    uint32_t id = 0, st = 0;
    if (!ReadWord(read, slot + kPlSlotIdOff, id) || !ReadWord(read, slot + kPlSlotStatusOff, st)) return false;
    if (id != kPlSlotEmptyId && (st == 0 || st == 2 || st == 3)) ++r.participants;
  }
  r.ok = true;
  out = r;
  return true;
}
inline std::string PlaneStructureText(const PlaneStructure& s) {
  return "structure " + std::to_string(s.a) + "/" + std::to_string(s.b) + "/" + std::to_string(s.c) + ", participants " + std::to_string(s.participants) + (s.loop_path ? "" : " (count unconfirmed: not the slot-loop path)");
}

inline PlaneWhy ReadPlaneFrame(ReadFn read, uint64_t hook_frame, rdr_scene::PlaneFrame& out, PlaneBadBlock* bad = nullptr) {
  using namespace rdr_scene;
  uint32_t mod = 0, enabled = 0, suppress = 0, holder = 0, cam_obj = 0;
  if (!read || !ReadWord(read, kPlaneModulePtr, mod)) return PlaneWhy::ReadFail;
  if (mod < 0x10000) return PlaneWhy::NoModule;
  if (!ReadWord(read, mod + kPlEnabledOff, enabled) || !ReadWord(read, kPlSuppressWord, suppress) || !ReadWord(read, kPlCamHolder, holder)) return PlaneWhy::ReadFail;
  if (enabled == 0) return PlaneWhy::Disabled;
  if (suppress != 0) return PlaneWhy::Suppressed;
  if (holder < 0x10000) return PlaneWhy::HolderNull;
  if (!ReadWord(read, holder + kPlCamHolderOff, cam_obj)) return PlaneWhy::ReadFail;
  if (cam_obj == 0) return PlaneWhy::NoCamera;
  uint32_t fade[10], cam[16], fit[16];
  if (!read(mod + kPlFadeOff, fade, sizeof fade) || !read(mod + kPlCameraOff, cam, sizeof cam) || !read(mod + kPlFitOff, fit, sizeof fit)) return PlaneWhy::ReadFail;
  PlaneFrame f;
  f.frame = uint32_t(hook_frame);
  f.have = kPlaneHaveAll;
  auto fl = [](uint32_t be) { const uint32_t w = Swap32(be); float v; std::memcpy(&v, &w, 4); return v; };
  for (int i = 0; i < 10; ++i) f.fade[i] = fl(fade[i]);
  for (int i = 0; i < 16; ++i) f.camera[i] = fl(cam[i]), f.fit_matrix[i] = fl(fit[i]);
  f.plane_normal[1] = -1.f;
  auto dump = [&](const float* v, uint32_t n) { if (bad) { bad->n = n; for (uint32_t k = 0; k < n && k < 16; ++k) bad->v[k] = v[k]; } };
  for (float v : f.fade) if (!std::isfinite(v)) { dump(f.fade, 10); return PlaneWhy::NonFiniteFade; }
  for (int i = 0; i < 16; ++i) if (!std::isfinite(f.camera[i])) { dump(f.camera, 16); return PlaneWhy::NonFiniteCamera; }
  {   // S103 PB: the game's x-collapse (sub_827ED960 divides 2 / 0): x scale and x offset non-finite, everything else finite = "no plane draw this frame" (the rectangle lies wholly off one side)
    bool any = false, others = true;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(f.fit_matrix[i])) { any = true; if (i != 0 && i != 3) others = false; }
    if (any && others && !std::isfinite(f.fit_matrix[0]) && !std::isfinite(f.fit_matrix[3])) {
      f.flags |= kPlaneNoDraw;
      f.have = kPlaneHaveAll & ~kPlaneHaveFit;
      out = f;
      return PlaneWhy::OffScreenFit;
    }
    if (any) { dump(f.fit_matrix, 16); return PlaneWhy::NonFiniteFit; }
  }
  out = f;
  return PlaneWhy::Ok;
}

// ---- S103 L2: which character slot is entering (the spot sits on ITS root, not always slot 0). The entrance-step states (src\scene_live: tools\input_scripts\states.toml entrance_step_N, docs history playbook 2 "Entrance
// steps found", confidence high, runs 829 / 830): the character of slot N is [0x836F38D0 + 4 N], its current act record [char + 28]; the first word of the act record is `act << 16 | step << 8 | ...` (c0_act 0x61A80500: act 25000
// = 0x61A8, the step byte 5). A slot is ENTERING when its act is 25000 and the step byte is >= 0 (-1 / unreadable = the scene resets). Exactly one entering slot is returned; none or several (a companion's own disk is [U]) = false:
// the latch then refuses the stamp and the decision walks.
inline bool ReadEntranceSlot(ReadFn read, uint32_t& slot) {
  if (!read) return false;
  int found = 0;
  for (uint32_t n = 0; n < 6; ++n) {
    uint32_t ch = 0, act = 0, word = 0;
    if (!ReadWord(read, kCharSlotTable + 4 * n, ch) || ch < 0x10000 || !ReadWord(read, ch + 28, act) || act < 0x10000 || !ReadWord(read, act, word)) continue;
    if ((word >> 16) == kEntranceAct && int8_t((word >> 8) & 0xFFu) >= 0) {
      slot = n;
      ++found;
    }
  }
  return found == 1;
}

// ---- kScCamera (INPUTS_BOARD.md K1): the frame's render camera, read at the latch.
// Source [C docs\systems\shadows.md 2, camera.md, caster_core.h kProductSlots]: sub_82BA7DE0(table 0x839299C0, cam) stores the camera pointer at table + 128 + 4 * slot and a per-slot override word at table + 136 + 4 * slot; the
// table has TWO slots (slot 0, 1). sub_82BA7748 returns the slot-0 pointer when the calling thread has no block ([[r13]+1048] null), which is the state of OUR latch reader. We read ONE named slot, 0, and
// never the override words or the 0x83929A68 global. Which slot the scene's draw thread uses is [U] until the trace INPUTS_BOARD.md K1 names; slot 1 is NOT read. The camera object holds the view at +240 (sub_829352D8 +EC),
// the projection at +176 (sub_82BB3748 +13C; the HUD ortho lands there too), near / far at +500 / +504 (the S-updater and the DoF converter read the same fields, post_effects.md 1). The scene composer sub_823C8548 -> sub_823C79A8 fills the camera from the shown
// game camera in the update, so at the latch it holds the frame's final camera.
// LAYOUT WRITTEN (Camera in scene_record.h): view and proj are the object's 16 floats in memory order, ROW-MAJOR, ROW-VECTOR convention: clip = [p 1] * view * proj; world -> view is p_view = p * M3 + t with
// t in view[12..14]; Y-down game space. The stream's constants c56-c59 are this product TRANSPOSED (lane R's rig transposes them). kCamViewIsViewProj stays clear, fov_y_deg is 0 (proj is given).
inline constexpr uint32_t kCamTable = 0x83929A40;   // pointer of slot 0 (table 0x839299C0 + 128)
inline constexpr uint32_t kCamView = 240, kCamProj = 176, kCamNear = 500, kCamFar = 504;
struct CameraInfo {
  uint32_t object = 0;    // the slot-0 camera object (guest address)
  uint32_t reason = 0;    // 0 = ok; 1 = no pointer; 2 = unreadable; 3 = view not rigid; 4 = projection pattern; 5 = near / far
};
inline bool ReadMat(ReadFn read, uint32_t guest, rdr_scene::Mat4& m) {
  uint32_t raw[16];
  if (!read || !read(guest, raw, sizeof raw)) return false;
  uint32_t w[16];
  for (int i = 0; i < 16; ++i) w[i] = Swap32(raw[i]);
  std::memcpy(m.m, w, sizeof w);
  return true;
}
// A rigid row-vector world -> view: the 3x3 rows orthonormal within 1e-3, column 3 = (0, 0, 0, 1) (zero and scaled matrices fail).
inline bool IsRigidView(const float* v) {
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(v[i])) return false;
  if (std::fabs(v[3]) > 1e-3f || std::fabs(v[7]) > 1e-3f || std::fabs(v[11]) > 1e-3f || std::fabs(v[15] - 1.f) > 1e-3f) return false;
  for (int i = 0; i < 3; ++i)
    for (int j = i; j < 3; ++j) {
      const float d = v[i * 4] * v[j * 4] + v[i * 4 + 1] * v[j * 4 + 1] + v[i * 4 + 2] * v[j * 4 + 2];
      if (std::fabs(d - (i == j ? 1.f : 0.f)) > 1e-3f) return false;
    }
  return true;
}
// A row-vector perspective: finite, clip.w = view z (m[11] = +-1, m[15] = 0), a non-zero focal pair, nothing off the pattern's zero cells.
inline bool IsPerspective(const float* m) {
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(m[i])) return false;
  if (std::fabs(std::fabs(m[11]) - 1.f) > 1e-3f || std::fabs(m[15]) > 1e-3f) return false;
  if (std::fabs(m[0]) < 1e-6f || std::fabs(m[5]) < 1e-6f) return false;
  return std::fabs(m[1]) < 1e-3f && std::fabs(m[4]) < 1e-3f && std::fabs(m[3]) < 1e-3f && std::fabs(m[7]) < 1e-3f && std::fabs(m[12]) < 1e-3f && std::fabs(m[13]) < 1e-3f;
}
inline bool ReadCamera(ReadFn read, rdr_scene::Camera& out, CameraInfo& info, uint32_t table = kCamTable, rdr_scene::Camera* raw_out = nullptr) {   // S103 S: table / raw_out for the diagnosis census only
  info = CameraInfo{};
  uint32_t obj = 0;
  if (!read || !ReadWord(read, table, obj)) return info.reason = 2, false;
  if (!obj) return info.reason = 1, false;
  info.object = obj;
  rdr_scene::Camera c;
  uint32_t nw = 0, fw = 0;
  if (!ReadMat(read, obj + kCamView, c.view) || !ReadMat(read, obj + kCamProj, c.proj) || !ReadWord(read, obj + kCamNear, nw) || !ReadWord(read, obj + kCamFar, fw)) return info.reason = 2, false;
  std::memcpy(&c.near_z, &nw, 4);
  std::memcpy(&c.far_z, &fw, 4);
  if (raw_out) *raw_out = c;
  if (!IsRigidView(c.view.m)) return info.reason = 3, false;
  if (!IsPerspective(c.proj.m)) return info.reason = 4, false;
  if (!(c.near_z > 0.f) || !(c.far_z > c.near_z) || !std::isfinite(c.far_z)) return info.reason = 5, false;
  const float* v = c.view.m;
  // eye_i = -sum_j t_j * M3[i][j]  (p_view = p_world * M3 + t, camera.md)
  for (int i = 0; i < 3; ++i) c.eye[i] = -(v[12] * v[i * 4 + 0] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
  c.fov_y_deg = 0;
  c.flags = 0;
  out = c;
  return true;
}

// S103 S (run 7259, camera diagnosis [C]): at the latch both slots of the camera table hold the 2D screen camera (view scale 2/1280, -2/720, proj the
// identity): the game has switched to its HUD camera by then. The scene camera is the composer's copy instead [C docs\systems\camera.md 54, 81-99;
// src\cinecam.h 33-69, live since Session 13]: the scene manager [0x836F3AA8] (sub_823C8548 copies the shown camera's slot, after the transition blend,
// to mgr +1488 (the 4x4 world -> view matrix, row-vector, the camera object's +240, the view) and +1552 (FOV, degrees: CineCam writes 60 there)
// every frame on the update side, before any walk. The projection is built by the reader from the FOV (kCamProjFromFov).
inline constexpr uint32_t kCamMgrPtr = 0x836F3AA8, kCamMgrView = 1488, kCamMgrFov = 1552;
inline bool ReadSceneCamera(ReadFn read, rdr_scene::Camera& out, CameraInfo& info, rdr_scene::Camera* raw_out = nullptr) {
  info = CameraInfo{};
  uint32_t mgr = 0, fw = 0;
  if (!read || !ReadWord(read, kCamMgrPtr, mgr)) return info.reason = 2, false;
  if (!mgr) return info.reason = 1, false;
  info.object = mgr;
  rdr_scene::Camera c;
  if (!ReadMat(read, mgr + kCamMgrView, c.view) || !ReadWord(read, mgr + kCamMgrFov, fw)) return info.reason = 2, false;
  float fov = 0;
  std::memcpy(&fov, &fw, 4);
  c.fov_y_deg = fov;
  if (raw_out) *raw_out = c;
  if (!IsRigidView(c.view.m)) return info.reason = 3, false;
  if (!(fov > 1.f && fov < 170.f)) return info.reason = 4, false;
  const float* v = c.view.m;
  for (int i = 0; i < 3; ++i) c.eye[i] = -(v[12] * v[i * 4 + 0] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
  c.flags = rdr_scene::kCamProjFromFov;   // proj stays the identity; near / far 0 = the reader's default [U]
  out = c;
  return true;
}

// S103 CAE: ONE shared camera read for the scene's latch readers: the composer's copy (ReadSceneCamera) and, when there is no scene manager at all (reason 1, the CAW editor), the draw thread's camera the
// game's own body draw reads (draw_camera_core.h, S103 CCF: Get on the RECORD clock). `draw_get(record_frame, camera)` is drawcam::GetForLatch; it is passed in because draw_camera_core.h includes this header.
// Used by inputs_contrib.h Take and particle_source.h (Take / TakeScreen). True with `info.reason = 0` for a camera from either source; `*from_draw` says which.
template <class DrawGet>
inline bool ReadSceneCameraOrDraw(ReadFn read, uint64_t record_frame, DrawGet&& draw_get, rdr_scene::Camera& out, CameraInfo& info, bool* from_draw = nullptr) {
  if (from_draw) *from_draw = false;
  if (ReadSceneCamera(read, out, info)) return true;
  if (info.reason == 1 && draw_get(record_frame, out)) {   // no scene manager: the draw thread's camera
    info.reason = 0;
    if (from_draw) *from_draw = true;
    return true;
  }
  return false;
}

// S103 PHS (docs\research\s103\PHS.md 3 / 4): is this frame an ENTRANCE (the pool rows are required)? Exactly when the game runs the pool draw sub_825BACE0: the stage-page pass object's entrance word [obj+88] != 0
// (ReadStagePageClock flags bit 1). 0 = unknown (the page object was not read: no stamp, the decision walks), 1 = not an entrance (the match, the CAW editor), 2 = an entrance. NOT the scene phase word: 4 is
// the game's generic "not the match" word, also written by the editor (seq 76, 0x83397CA0) and the menu sequences. The phase word still drives every LOOK test (== 1 match look: the game's own rule).
// Plant (unit only): 1 = the old rule, the phase word (`phase != 1` = entrance).
inline int g_entrance_plant = 0;
inline int PageEntranceState(bool page_read, uint32_t page_flags, bool phase_readable, uint32_t phase) {
  if (g_entrance_plant == 1) return !phase_readable ? 0 : phase != 1 ? 2 : 1;
  if (!page_read) return 0;
  return (page_flags & 2u) ? 2 : 1;
}
// The kInStagePools stamp: a known frame that is not an entrance, or an entrance whose rows exist and were derived.
inline bool PoolsStamp(int entrance_state, bool rows_exist, bool ent_ok) { return entrance_state != 0 && (entrance_state != 2 || (rows_exist && ent_ok)); }

// kScInputStamps: one row per input that has been stamped at least once.
// (the caller snapshots this in the latch, never at Emit: the counters are written by game threads)
inline void FillStamps(std::vector<rdr_scene::InputStampRow>& out) {
  for (const Info& i : kInputs) {
    const uint64_t f = g_counters.last[i.bit].load(std::memory_order_relaxed);
    if (!f && !g_counters.stamps[i.bit].load(std::memory_order_relaxed)) continue;
    rdr_scene::InputStampRow r;
    r.bit = uint8_t(i.bit), r.lag = uint8_t(i.lag), r.moved = i.moved ? 1 : 0, r.clock = kClockHook, r.frame = f;   // the stamp is in the HOOK clock
    out.push_back(r);
  }
}

// S103 L1C review B1: the bits that are stamped at EMIT, not at the latch. The latch snapshot (FillStamps, taken at the end of the latch) cannot carry them, so BuildFrame overlays these rows from
// the counters after the emit (live_scene.cpp SetEmitStampFill). Only bit 14 (screens: it needs the record's own movie rows). Every other bit is stamped at the latch; tools\scene\emit_stamp_lint.py
// fails any scene_inputs::Capture inside an Emit that is not named here.
inline constexpr uint32_t kEmitStampedBits = 1u << kInStageScreens;
inline void FillEmitStamps(std::vector<rdr_scene::InputStampRow>& out) {
  for (const Info& i : kInputs) {
    if (!((kEmitStampedBits >> i.bit) & 1u)) continue;
    const uint64_t f = g_counters.last[i.bit].load(std::memory_order_relaxed);
    if (!f && !g_counters.stamps[i.bit].load(std::memory_order_relaxed)) continue;
    rdr_scene::InputStampRow r;
    r.bit = uint8_t(i.bit), r.lag = uint8_t(i.lag), r.moved = i.moved ? 1 : 0, r.clock = kClockHook, r.frame = f;
    out.push_back(r);
  }
}

// The census line: `[scene][inputs] moved N of M, post P (last F), models ...`. Strings are built here, at print time only.
inline std::string CensusLine() {
  std::string s = "[scene][inputs] moved ";
  int moved = 0;
  for (const Info& i : kInputs) moved += i.moved ? 1 : 0;
  s += std::to_string(moved) + " of " + std::to_string(int(kInputCount));
  for (const Info& i : kInputs) {
    s += ", ";
    s += i.name;
    s += i.moved ? " " : " (open) ";
    s += std::to_string((unsigned long long)g_counters.stamps[i.bit].load(std::memory_order_relaxed));
    s += "/f" + std::to_string((unsigned long long)g_counters.last[i.bit].load(std::memory_order_relaxed));
  }
  s += " [blocks the frame:";   // S103 A3: every registered input without a reader, with its owner
  bool any = false;
  for (const Info& i : kInputs)
    if (!i.moved) s += (any ? ", " : " "), s += i.name, s += " (", s += i.owner, s += ")", any = true;
  s += any ? "]" : " none]";
  s += " [held by --wwe2k14_rdr_scene_hold:";   // S103 L1C: the run-time hold and what it swallowed
  bool held_any = false;
  for (const Info& i : kInputs)
    if (Held(i.bit)) s += (held_any ? ", " : " ") + std::string(i.name) + " (" + std::to_string((unsigned long long)g_held_drops[i.bit].load(std::memory_order_relaxed)) + " stamps dropped)", held_any = true;
  s += held_any ? "]" : " none]";
  return s;
}

}  // namespace scene_inputs
