// Lane E (Session 103): helpers shared by live_maps.cpp, live_map_shadow.cpp and live_map_circles.cpp. Not an interface for other lanes.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../renderer/shaders/spv/fx_registry.h"
#include "caster_cull.h"   // S111 CASTCULL: the caster passes' face cull (kCasterCull, kCasterFrontShadow / kCasterFrontPlane)

namespace rdr_scene::maps_detail {

// Index of the fx variant called `name` (kVariants), or -1.
inline int FindVariant(const char* name) {
  for (size_t i = 0; i < std::size(rdr::fx::kVariants); ++i)
    if (std::strcmp(rdr::fx::kVariants[i].name, name) == 0) return int(i);
  return -1;
}

// Writes `vec4s` registers starting at game register `reg` into a packed constant block of variant `v` (as the block lays its ranges out); false when the variant has no such range.
inline bool PutRegisters(std::vector<uint8_t>& block, const rdr::fx::Variant& v, uint32_t reg, const float* data, uint32_t vec4s) {
  for (uint32_t r = 0; r < v.constant_count; ++r) {
    const rdr::fx::ConstantRange& c = v.constants[r];
    if (c.reg != reg) continue;
    const uint32_t n = vec4s < c.count ? vec4s : c.count;
    if (size_t(c.offset) + size_t(n) * 16 > block.size()) return false;
    std::memcpy(block.data() + c.offset, data, size_t(n) * 16);
    return true;
  }
  return false;
}

inline void Transpose4(const float* m, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[c * 4 + r] = m[r * 4 + c];
}

}  // namespace rdr_scene::maps_detail

namespace rdr_scene {

// S103 E2 (review 2): moved here from live_maps.cpp so every map picks its stamp with the same rule. The ONE stamp of a record: `header - 1` when an entry carries it (measured: SF1 run 7147), else the newest
// stamp at or below the header and within `window` (kStampWindow = 4) of it. Never a future stamp. Returns false when no stamp qualifies.
template <class It, class Get>
bool ChooseStamp(It first, It last, Get stamp_of, uint32_t header, uint32_t& out, uint32_t window = 4) {
  bool have_exact = false, have_any = false;
  uint32_t best = 0;
  for (It it = first; it != last; ++it) {
    const uint32_t s = stamp_of(*it);
    const uint32_t below = header - s;   // wraps for a future stamp: huge
    if (below > window) continue;        // future or too old
    if (s == header - 1) have_exact = true;
    if (!have_any || int32_t(s - best) > 0) best = s, have_any = true;
  }
  if (!have_any) return false;
  out = have_exact ? header - 1 : best;
  return true;
}

inline bool Finite16(const float* m) {
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(m[i])) return false;
  return true;
}

}  // namespace rdr_scene
