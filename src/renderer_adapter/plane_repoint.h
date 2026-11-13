// lane 195 (GATE_FAULTS row 1, the title-logo glitch): the flush-driven movie plane image and a re-pointed texture object.
// Pure (no reader types) so the decision has an offline test: src/renderer_adapter/plane_repoint_test.cpp.
// A flush-driven plane is read only after the decoder's flush names it. That is right while the binding is the same memory; when the game
// points the same texture object at other memory or another size (the next movie's planes, a reused object), the image is a NEW picture:
// it leaves the flush-driven rule (the page watch covers it) until a flush names the new base, which sets the flag again (NameByFlush).
#pragma once
#include <cstdint>

namespace rdr_adapter::plane {

struct State {
  uint32_t key[2] = {0, 0};   // base page and size word of the plane as last bound
  bool content_written = false;
  bool flushed = false;
  bool flush_driven = false;  // sticky per BINDING: set by a flush naming the image, cleared when the binding moves
};

struct Result {
  bool moved = false;  // the binding moved (any bind with another base page / size after a first one): census flush_driven_plane_repointed
  bool stale = false;  // lane 172's condition: the old code would have served cached bytes for a moved binding (census ..._stale): the evidence for the glitch's cause
};

// Call on each bind of a flush-driven image with the texture header words. `legacy`: the old sticky behaviour (only the key and the census run).
inline Result Rebind(State& s, const uint32_t w[6], bool legacy = false) {
  Result r;
  const uint32_t base = w[1] & 0xFFFFF000u, size = w[2];
  if (s.key[0] == base && s.key[1] == size) return r;
  r.moved = s.key[0] != 0;
  r.stale = r.moved && s.content_written && !s.flushed;
  s.key[0] = base;
  s.key[1] = size;
#ifdef WWE2K14_PLANT_STICKY  // plant: the old sticky behaviour (the unit test must fail with it)
  legacy = true;
#endif
  if (r.moved && !legacy) {
    s.content_written = false;
    s.flushed = false;
    s.flush_driven = false;
  }
  return r;
}

// The flush-driven early return: true = serve the cached content, read nothing.
inline bool ServeCached(const State& s) { return s.flush_driven && s.content_written && !s.flushed; }

// OnPlaneFlush's naming step: every image in `by_base` (physical base -> image id, ordered) whose base lies in [phys - 4 KB, phys + size) is
// passed to name(id), which sets flushed + flush_driven on it and returns false to skip it (a dead image). True if at least one was named.
template <class Map, class F>
inline bool NameByFlush(const Map& by_base, uint32_t phys, uint32_t size, F&& name) {
  const uint32_t begin = phys >= 0x1000u ? phys - 0x1000u : 0, end = phys + size;
  bool named = false;
  for (auto it = by_base.lower_bound(begin); it != by_base.end() && it->first < end; ++it) named |= name(it->second);
  return named;
}

}  // namespace rdr_adapter::plane
