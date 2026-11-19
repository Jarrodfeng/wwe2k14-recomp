// S103 MBC review 3: the prop caster cache (LiveMapsHook::prop_casters_) is keyed by front-end model id, and ids are never reused, so entries of finished entrances / matches would pile up (a GPU vertex + index buffer each).
// Rule: an entry not used for more than `max_age` frames is retired and erased. Pure (the unit tools\weapons\mapb_test.cpp runs the same code on a mock entry type). No game or SDK includes.
#pragma once
#include <cstdint>
#include <unordered_map>

namespace rdr_scene {
inline constexpr uint64_t kPropCasterMaxAge = 60;
// `Map` = a map whose mapped value is a pointer-like with a `last_used` member; `retire(*entry)` frees what the entry holds. Returns how many were retired.
template <class Map, class RetireFn>
inline size_t SweepStaleProps(Map& m, uint64_t now, uint64_t max_age, RetireFn retire) {
  size_t n = 0;
  for (auto it = m.begin(); it != m.end();) {
    if (now > it->second->last_used && now - it->second->last_used > max_age) {
      retire(*it->second);
      it = m.erase(it);
      ++n;
    } else {
      ++it;
    }
  }
  return n;
}
}  // namespace rdr_scene
