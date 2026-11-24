// S103 B3c: the card quad's light-map gain rule as pure functions (the C++ unit tools\scene\b3_card_unit.cpp runs THESE, the front end and the record -> frame library call them).
// The game's rule (sub_827F22C8 0x827F23B8, docs\research\s103\T3b_lightmap_writer.md): a card-technique draw's c184.z = the arena gain x (float)[0x8376383C] * 0.001, applied only when that product factor is > 0;
// a missing row or a word of 0 leaves the gain unscaled.
#pragma once
#include <vector>

#include "rows_stage_look.h"

namespace rdr_scene::card_gain {

// The record's kSlCardFactor row (flags bit 0 = the word was read; dir[0] = the factor) -> (valid, factor). No row = (false, 0).
inline void FromRows(const std::vector<StageLookRow>& rows, bool& valid, float& factor) {
  valid = false, factor = 0.f;
  for (const StageLookRow& r : rows)
    if (r.kind == kSlCardFactor && (r.flags & 1)) factor = r.dir[0], valid = true;
}
// The factor a frame draws with: 0 = none (no row, or a word <= 0).
inline float Effective(bool valid, float factor) { return (valid && factor > 0.f) ? factor : 0.f; }
// A model's gain: a card-technique model takes the table's gain x the effective factor when it is > 0; every other model keeps the table's gain.
inline float Gain(float base_gain, bool card_model, float effective) { return base_gain * ((card_model && effective > 0.f) ? effective : 1.f); }   // [f32]

}  // namespace rdr_scene::card_gain
