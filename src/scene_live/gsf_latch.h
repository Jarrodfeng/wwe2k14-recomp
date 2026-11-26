// S103 FXF: the 'gGsf' node's state, latched for the post row (docs\research\s103\FXP.md adversary C; the review's fixes 2, 3, 7). The component's per-frame node method sub_825B7A38 (vtable 0x8201F014 slot 1)
// writes the monotone words S+208..+220 every frame from its index (component +16) and its blend t (component +20): index -1 = the arena's OWN row (the 30 Years sepia, x 0.01), >= 0 = a moment tint
// (move-event types 31-40) = lerp(arena row, preset, t) x 0.01. The game thread notes {index, t, the four words it wrote} after each call (src\gsf_tint_hook.h); the scene latch THREAD calls Fill, which writes
// PostParamsRow::gsf_code (1 = the arena sepia; 2 = a tint; index + 2 for index >= 0; 0 = unknown), gsf_t, and the arena row's four words (fx[]). A write by another writer after the node (the camera-effect classes
// sub_8275E9E0 / sub_8275ECB0, the cue code sub_822952A8) leaves S+208..+220 different from the node's words: that is a tint, not the sepia. Pure header: no game, SDK or renderer includes (the offline unit builds it).
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>

#include "../renderer_scene/rows_inputs.h"
#include "post_settings.h"

namespace scene_inputs::gsf {

inline constexpr int32_t kNoIndex = INT32_MIN;   // BHF4: no node call since a stage change (0 is a real moment-tint index)
struct State {
  uint32_t calls = 0;
  int32_t index = 0;
  float t = 0.f;
  uint32_t node[4] = {};        // S+208, +212, +216, +220 as the node left them
  bool arena_known = false;     // an index -1 call was seen: `arena` = its four words (flag, Cb, Cr, Alpha, each x 0.01) = the arena's own row
  uint32_t arena[4] = {};
};
inline std::mutex g_mu;   // the node runs on a game thread, the latch on the latch thread; once a frame each
inline State g_state;

inline void Note(int32_t index, float t, const uint32_t node_words[4]) {
  std::lock_guard<std::mutex> lk(g_mu);
  ++g_state.calls, g_state.index = index, g_state.t = t;
  std::memcpy(g_state.node, node_words, 16);
  if (index == -1) g_state.arena_known = true, std::memcpy(g_state.arena, node_words, 16);
}

// BHF4 (BH2 G4): a stage change forgets the previous arena's row (arena_known / arena[] / node words), or the next arena's flag-off check would read the old arena's sepia. `id` = the stage object's id
// ([stage + 1216]); an unreadable stage (ok false) changes nothing. The first known id only records it. Pure (the unit calls it).
struct StageMark { bool known = false; uint32_t id = 0; };
inline StageMark g_stage;
inline bool NoteStageOn(State& st, StageMark& m, bool ok, uint32_t id) {
  if (!ok) return false;
  const bool changed = m.known && m.id != id;
  m.known = true, m.id = id;
  if (changed) st.arena_known = false, std::memset(st.arena, 0, 16), std::memset(st.node, 0, 16), st.index = kNoIndex, st.t = 0.f;
  return changed;
}
inline bool NoteStage(bool ok, uint32_t id) {
  std::lock_guard<std::mutex> lk(g_mu);
  return NoteStageOn(g_state, g_stage, ok, id);
}

// What the latch derives from the state and the row's own S words (pure: the unit calls it with made-up states).
struct Result {
  uint32_t code = 0;     // PostParamsRow::gsf_code
  float t = 0.f;
  uint32_t arena[4] = {};
  bool arena_known = false;
};
inline Result Classify(const State& st, bool fresh, const uint32_t s208[4]) {
  Result r;
  r.arena_known = st.arena_known;
  std::memcpy(r.arena, st.arena, 16);
  r.t = st.t;
  if (!fresh) {   // no node call lately: unknown; with the arena's flag known to be OFF any monotone is a moment tint (review 3)
    r.code = (st.arena_known && st.arena[0] == 0) ? 2u : 0u;
    return r;
  }
  const bool same = std::memcmp(s208, st.node, 16) == 0;   // S still holds exactly the node's words
  if (st.index < -1) {   // BHF4: forgotten at a stage change: unknown until the node calls again
    r.code = 0u;
    return r;
  }
  if (st.index == -1) r.code = same ? 1u : 2u;              // index -1 + untouched = the arena sepia; another writer's words after the node = a tint
  else r.code = uint32_t(st.index) + 2u;
  return r;
}

// The latch thread's call. A latch with no node call since the previous one is "stale" after two repeats (a removed component never leaves a code). (S111 HEADGUARD review: Peek, the off-latch take's reader of the cached result, went with that take.)
struct Latcher {
  uint32_t seen = 0;
  int stale = 0;
  Result last;
};
inline Latcher g_latcher;
inline void Apply(rdr_scene::PostParamsRow& row, const Result& r) {
  row.gsf_code = r.code;
  float t = r.t;
  std::memcpy(&row.fx[rdr_scene::kFxGsfT], &t, 4);
  for (int i = 0; i < 4; ++i) row.fx[rdr_scene::kFxArena + i] = r.arena_known ? r.arena[i] : 0u;
}
inline void Fill(rdr_scene::PostParamsRow& row) {   // LATCH THREAD ONLY (review 7)
  std::lock_guard<std::mutex> lk(g_mu);
  Latcher& l = g_latcher;
  bool fresh;
  if (g_state.calls != l.seen) l.seen = g_state.calls, l.stale = 0, fresh = true;
  else fresh = ++l.stale <= 2;
  if (!fresh && l.stale > 2 && g_state.arena_known) g_state.arena_known = false, std::memset(g_state.arena, 0, 16);   // BHF4 (BH2 G4): no node for 3+ latches (a scene with no stage, an early return from OnLightSets): the arena row is forgotten
  uint32_t s208[4];
  for (int i = 0; i < 4; ++i) s208[i] = row.s[(rdr_scene::post_settings::kMonotone / 4) + i];
  l.last = Classify(g_state, fresh, s208);
  Apply(row, l.last);
}
}  // namespace scene_inputs::gsf
