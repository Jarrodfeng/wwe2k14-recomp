// Bit 7 (caster_entries): when the latch may stamp it (Session 103, lane E4). Pure decision + the one call; the switch is caster_stamp_flag.h (OFF).
// The rule (review 2 of lane E3): stamp only when the shadow fit's inputs for the frame exist in the record: the key light of EVERY wrestler model from the light objects (kLoModelMap install ->
// the game's selection rule -> the object's R dir 4, light_install.h) and at least one wrestler model; the casters' skeletons are bit 3's (stamped separately and required separately).
#pragma once
#include <cstdint>
#include <vector>

#include "../renderer_scene/light_install.h"
#include "caster_stamp_flag.h"
#include "inputs_core.h"

namespace rdr_scene::live::caster_stamp {

// Pure: ready when every wrestler model in `models` has a key light in `rows`; `why` names the first model that does not.
inline bool Ready(const std::vector<LightObjectRow>& rows, const std::vector<uint32_t>& models, const char*& why) {
  float key[3];
  return CasterInputsReady(rows, models.data(), models.size(), key, why);
}

// The latch's call (inputs_contrib.h, after the wrestler models are known): a no-op while the switch is off. Returns true when it stamped.
inline bool MaybeStamp(const std::vector<LightObjectRow>& rows, const std::vector<uint32_t>& models, uint64_t hook, const char** why_out = nullptr) {
  if constexpr (!kStampCasterEntries) {
    (void)rows, (void)models, (void)hook, (void)why_out;
    return false;
  } else {
    const char* why = "";
    const bool ok = Ready(rows, models, why);
    if (why_out) *why_out = why;
    if (ok) scene_inputs::Capture(scene_inputs::kInCasterEntries, hook);
    return ok;
  }
}

}  // namespace rdr_scene::live::caster_stamp
