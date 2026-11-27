// The wrestler actor test and the frame's wrestler-light stamp from a scene record (Session 103, lanes E3 / E8). Pure: record_io.h + scene_record.h only; header only. No allocation.
// S111 KEYDEAD: KeyLightFromRecord and RecordHasWrestler are removed (the fitted shadow reads no key light since SHADOWKEY; nothing called them).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>   // S103 E8
#include <cstring>

#include "light_install.h"   // S103 E4
#include "record_io.h"
#include "scene_record.h"
#include "rows_inputs.h"   // S106 G4-2
#include "rows_lights.h"

namespace rdr_scene {

// A wrestler's skeleton has 99 / 109 palette nodes, a referee's / manager's 79 [C: brock_losswin f110 objects actor:slot:0 / 6 vs 7 / 8]; at least this many marks "a wrestler" [L].
inline constexpr uint32_t kShadowFitWrestlerBones = 90;
inline constexpr uint32_t kShadowFitStampWindow = 4;   // = kStampWindow (live_maps.h): the rule is the same, copied so this header stays pure

// Is `o` a wrestler actor (a skinned "actor..." object with a wrestler-sized palette)?
inline bool IsWrestlerActor(const SceneTables& t, const Object& o) {
  if (!(o.flags & kObjSkinned) || o.bones_count < kShadowFitWrestlerBones || !o.id) return false;
  return std::strncmp(t.Str(o.id).data(), "actor", 5) == 0;
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

}  // namespace rdr_scene
