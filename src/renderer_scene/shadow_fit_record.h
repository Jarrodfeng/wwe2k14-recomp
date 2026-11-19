// The wrestlers' key light vector from a scene record (Session 103, lane E3; shadow_fit.h feeds on it). Pure: record_io.h + scene_record.h only; header only. No allocation.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>   // S103 E8
#include <cstring>

#include "light_install.h"   // S103 E4
#include "record_io.h"
#include "scene_record.h"

namespace rdr_scene {

// A wrestler's skeleton has 99 / 109 palette nodes, a referee's / manager's 79 [C: brock_losswin f110 objects actor:slot:0 / 6 vs 7 / 8]; at least this many marks "a wrestler" [L].
inline constexpr uint32_t kShadowFitWrestlerBones = 90;
inline constexpr uint32_t kShadowFitStampWindow = 4;   // = kStampWindow (live_maps.h): the rule is the same, copied so this header stays pure

// Is `o` a wrestler actor (a skinned "actor..." object with a wrestler-sized palette)?
inline bool IsWrestlerActor(const SceneTables& t, const Object& o) {
  if (!(o.flags & kObjSkinned) || o.bones_count < kShadowFitWrestlerBones || !o.id) return false;
  return std::strncmp(t.Str(o.id).data(), "actor", 5) == 0;
}
// A record with a wrestler to cast a shadow (the shadow map is expected).
inline bool RecordHasWrestler(const SceneTables& t) {
  for (const Object& o : t.objects)
    if (IsWrestlerActor(t, o)) return true;
  return false;
}

// S103 E8: a light ref of a wrestler actor (its state exists), and THE stamp of the frame (rf_wrestlers.cpp lights each wrestler from the same stamp the key is read at): header - 1 when a wrestler's ref carries it,
// else the newest within the window, never a future or the oldest one. False = no wrestler light state in the window.
inline bool WrestlerRef(const SceneTables& t, const LightRef& r) {
  if (r.state >= t.light_states.size() || t.engine_objects.size() != t.objects.size()) return false;
  for (size_t i = 0; i < t.objects.size(); ++i)
    if (t.engine_objects[i] == r.object && IsWrestlerActor(t, t.objects[i])) return true;
  return false;
}
inline bool WrestlerLightStamp(const SceneTables& t, uint32_t& stamp) {
  const uint32_t header = uint32_t(t.header.guest_frame);
  bool have_exact = false, have_any = false;
  uint32_t best = 0;
  for (const LightRef& r : t.light_refs) {
    if (header - r.frame > kShadowFitStampWindow || !WrestlerRef(t, r)) continue;   // future (wraps huge) or too old, or not a wrestler
    if (r.frame == header - 1) have_exact = true;
    if (!have_any || int32_t(r.frame - best) > 0) best = r.frame, have_any = true;
  }
  stamp = have_exact ? header - 1 : best;
  return have_any;
}

// The wrestlers' KeyLightVec from the record. The game's wrestler colour draw reads it from the render camera record R (R+16+16*4, "light 4" of the model's installed light object); the record
// carries R as kScLightStates and names which state each model call read in kScLightRefs. ONE stamp is chosen (header - 1 when a wrestler's ref carries it, else the newest within the window, never
// a future or the oldest one), then the first wrestler's ref of that stamp. A record with no light states has NO key light: a counted hole, never a guess.
// NOTE (review 2, updated S103 probe KEY): the walk's model-entry hook (0x82C04E1C) writes kScLightStates on walked frames; on OURS frames the A8 / L1C wrestler-light latch
// (wrestler_light_latch.h, kWrestlerLightFromLatch) adds a synthetic state per actor slot from the character's embedded light object (char+3984), so OURS frames carry the key too
// (match run 028: synth 62521, latch==walk differ 0; CAW editor run 031: synth up to 1195). A record with no state for the frame is the rare transient hole below.
inline bool KeyLightFromRecord(const SceneTables& t, float key[3], const char*& why, bool allow_objects = kKeyFromLightObjects) {
  const char* obj_why = "";
  const auto from_objects = [&]() {   // review F1: ONLY behind kKeyFromLightObjects (OFF), ONLY when no wrestler's light state is in the window; never overrides a walked state
    if (!allow_objects || t.light_objects.empty() || !RecordHasWrestler(t)) return false;
    return KeyLightOfWrestler(t.light_objects, key, obj_why) ? (why = obj_why, true) : false;
  };
  if (t.light_states.empty() && t.light_objects.empty()) {
    why = "the record has no light states and no light objects (the wrestlers' key light needs lane A's light rows: W2 / L1-L3)";
    return false;
  }
  if (t.light_states.empty()) {
    if (from_objects()) return true;
    why = obj_why[0] ? obj_why : "the record has light objects (id 32) but the key light from them is off (kKeyFromLightObjects) or not resolvable: no key light";
    return false;
  }
  uint32_t stamp = 0;
  if (!WrestlerLightStamp(t, stamp)) {   // no wrestler light state in the window: the objects may answer (when allowed); a walked state is never overridden
    if (from_objects()) return true;
    why = "no light state of this frame's window is joined to a wrestler (LightRef): no key light";
    return false;
  }
  for (const LightRef& r : t.light_refs) {
    if (r.frame != stamp || !WrestlerRef(t, r)) continue;
    const float* k = &t.light_states[r.state].light[kLsDir + 4 * 4];
    if (!IsKeyDirection(k)) continue;   // S103 E8: the shared key test (light_install.h): a unit direction with an azimuth; the w lane is no key marker (run 28: w = 0 on every light of a wrestler state)
    key[0] = k[0], key[1] = k[1], key[2] = k[2];
    why = "light state read by a wrestler's model call (R dir 4)";
    return true;
  }
  why = "no usable key in this frame's wrestler light states: no key light";
  return false;
}

}  // namespace rdr_scene
