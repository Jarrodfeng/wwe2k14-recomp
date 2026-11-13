// wwe2k14 - the "forced scene" marker (S101 lane 32). A frame reached by forcing a scene (src/scene_director.h: !scene menu / mid / victory)
// says so, so a fault seen only in a forced state is known to be one: the case zip's manifest carries where.forced = {verb, id, frame}
// (src/bookmark.h FillCaseManifest), and tools/case/intake.py prints it on line 2. No game or SDK includes: plain atomics.
#pragma once

#include <atomic>
#include <cstdint>

namespace wwe2k14::scene_forced {

enum Verb : int32_t { kNone = 0, kMenu = 1, kMid = 2, kVictory = 3, kAfter = 4, kFire = 5, kStep = 6 };  // S101 lane 108: kFire = `!wm30 fire BANK` (src/wm30_fire.h); S101 lane 121: kStep = `!wm30 step K` (id = the step)

inline std::atomic<int32_t> g_verb{kNone};
inline std::atomic<int32_t> g_id{0};
inline std::atomic<uint64_t> g_frame{0};

inline void Set(Verb v, int32_t id, uint64_t frame) {
  g_id.store(id);
  g_frame.store(frame);
  g_verb.store(v);
}
inline void Clear() { g_verb.store(kNone); }

inline const char* Name(int32_t v) { return v == kMenu ? "menu" : v == kMid ? "mid" : v == kVictory ? "victory" : v == kAfter ? "after" : v == kFire ? "fire" : v == kStep ? "step" : "none"; }

// false = nothing forced.
inline bool Get(const char*& verb, int32_t& id, uint64_t& frame) {
  const int32_t v = g_verb.load();
  if (v == kNone) return false;
  verb = Name(v);
  id = g_id.load();
  frame = g_frame.load();
  return true;
}

}  // namespace wwe2k14::scene_forced
