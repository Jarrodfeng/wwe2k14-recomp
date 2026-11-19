// S103 L31A: the arena set's rebuild debounce (EnsureArena, rf_c_core.cpp). The signature (slots, moving, screens) can flap for a frame or two at a look flip or a movie loop;
// every rebuild retires a whole GPU set. The set is rebuilt only after `k` CONSECUTIVE guest frames showed the same NEW signature; meanwhile the old set keeps drawing.
// Pure logic (no game, no GPU): unit-tested by tools\renderer\own_nodes_unit.cpp.
#pragma once
#include <cstdint>

namespace rdr_scene::rf::c {

inline constexpr uint32_t kRebuildDebounceFrames = 8;

struct SigDebounce {
  uint64_t pending = 0, first_frame = 0, last_frame = 0;
  uint32_t seen = 0;
  bool active = false;
  // `current` = the signature of the built set, `sig` = this frame's. True = rebuild now. A call for a frame already counted does not count twice (several appliers ask in one frame).
  bool Step(uint64_t current, uint64_t sig, uint64_t frame, uint32_t k = kRebuildDebounceFrames) {
    if (sig == current) { active = false; return false; }
    if (!active || pending != sig) { active = true, pending = sig, seen = 1, first_frame = last_frame = frame; return k <= 1; }
    if (frame == last_frame) return seen >= k;
    if (frame != last_frame + 1) { seen = 1, first_frame = frame; last_frame = frame; return k <= 1; }   // a gap: not consecutive
    last_frame = frame, ++seen;
    return seen >= k;
  }
};

}  // namespace rdr_scene::rf::c
