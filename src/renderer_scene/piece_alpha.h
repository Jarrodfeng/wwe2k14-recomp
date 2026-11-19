// S103 EYF: the model alpha (MdlAlpha = VS c185.x = [model+252]) of a wrestler's own-skeleton piece (hair, caps, glasses, the eyelid / lash layer, attire PACH entry 20002).
// The game fades each piece with its OWN model's alpha; the owner's body draws the piece's meshes here, so the piece's value is carried to the owner (docs\research\s103\EYES.md).
// Pure functions over a record: rf_wrestlers.cpp (the carry) and tools\scene\eyes_unit.cpp (the offline proof) share them.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "record_io.h"
#include "scene_record.h"

namespace rdr_scene::rf {

struct PieceAlphaValue {
  float alpha = 0.f;   // what the owner draws the piece with
  bool found = false;  // a kScModelRuntime row of the piece's engine object with kMrAlphaRead was in the record
};

// FAIL CLOSED: a piece with no engine address, or no readable row, is alpha 0 (not drawn), never 1. `plant_one` is the proof's twin (the bug: every piece at alpha 1).
inline PieceAlphaValue PieceModelAlpha(const SceneTables& t, size_t obj_index, bool plant_one = false) {
  PieceAlphaValue r;
  if (plant_one) return PieceAlphaValue{1.f, true};
  const uint32_t addr = obj_index < t.engine_objects.size() ? t.engine_objects[obj_index] : 0u;
  if (!addr) return r;
  for (const ModelRuntimeRow& m : t.model_runtime) {
    if (m.object != addr || !(m.flags & kMrAlphaRead)) continue;
    r.found = true;
    r.alpha = std::isnan(m.alpha) ? 0.f : (m.alpha < 0.f ? 0.f : (m.alpha > 1.f ? 1.f : m.alpha));   // the last matching row wins
  }
  return r;
}

}  // namespace rdr_scene::rf
