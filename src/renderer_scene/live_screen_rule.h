// S103 SCX (SCG.md): when the stage screens' live grab (FrontEnd slot 800 / 801-809 idle screens) is forgotten. Header only, no includes of ours.
// The game binds its grab texture whenever the object exists and never blanks it (sub_8276A808 / sub_82C27F88), so a gap in drawn guest frames
// (a mailbox drop, the decoupled 30, a DrawLive miss) keeps the last grab. Only these three facts clear it: a scene change (a stageless record), another
// arena (rf::ArenaEpoch moved), a target size change.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace rdr_scene {

enum class LiveScreenWhy : uint8_t { None = 0, SceneChange, ArenaChange, SizeChange, Count };

inline const char* LiveScreenWhyName(LiveScreenWhy w) {
  switch (w) {
    case LiveScreenWhy::SceneChange: return "scene_change";
    case LiveScreenWhy::ArenaChange: return "arena_change";
    case LiveScreenWhy::SizeChange: return "size_change";
    default: return "none";
  }
}

// The rule. A drawn-frame gap is deliberately not an input.
struct LiveScreenFacts {
  bool stageless_record = false;   // DrawLive: "no stage in the record (scene change)"
  bool arena_changed = false;      // rf::ArenaEpoch differs from the last one DrawLive saw
  bool size_changed = false;       // FrontEnd::EnsureTargets recreated the grab
};
inline LiveScreenWhy LiveScreenInvalidation(const LiveScreenFacts& f) {
  if (f.size_changed) return LiveScreenWhy::SizeChange;
  if (f.stageless_record) return LiveScreenWhy::SceneChange;
  if (f.arena_changed) return LiveScreenWhy::ArenaChange;
  return LiveScreenWhy::None;
}

// The S103 M (N3) rule this replaces, kept only as the unit's planted twin (never called by the game).
inline bool OldGapRuleInvalidates(bool have_drawn, uint64_t gf, uint64_t last_drawn_gf) {
  return !have_drawn || (gf != last_drawn_gf + 1 && gf != last_drawn_gf);
}

// Census: the invalidations that took a valid grab away, by reason (relaxed atomics, read by the [host][cpu] line).
struct LiveScreenCensus {
  std::atomic<uint64_t> by[size_t(LiveScreenWhy::Count)]{};
};
inline LiveScreenCensus& LiveScreenCensusRef() {
  static LiveScreenCensus c;
  return c;
}
inline void NoteLiveScreenInvalid(LiveScreenWhy w) { LiveScreenCensusRef().by[size_t(w)].fetch_add(1, std::memory_order_relaxed); }
inline std::string LiveScreenCensusText() {
  const LiveScreenCensus& c = LiveScreenCensusRef();
  uint64_t tot = 0;
  for (size_t i = 1; i < size_t(LiveScreenWhy::Count); ++i) tot += c.by[i].load(std::memory_order_relaxed);
  if (!tot) return std::string();
  std::string s = "; live_screen_invalidated " + std::to_string((unsigned long long)tot) + " (";
  for (size_t i = 1; i < size_t(LiveScreenWhy::Count); ++i)
    s += std::string(i > 1 ? " " : "") + LiveScreenWhyName(LiveScreenWhy(i)) + " " + std::to_string((unsigned long long)c.by[i].load(std::memory_order_relaxed));
  return s + ")";
}

}  // namespace rdr_scene
