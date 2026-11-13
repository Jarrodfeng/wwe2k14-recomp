// wwe2k14 - the flow watch of `!wm30 fire` (S101 lane 108): while a fired scene is being followed, the in-game flow's state changes are logged
// as proof lines. The two hooks that see them (sub_827A9AE0 SetState, sub_827BDA68 next-state pick) already live in src/flow_probe.h; a second
// REX_HOOK on the same function would collide, so flow_probe.h calls these two functions behind one relaxed load. Log only.
#pragma once

#include <rex/logging.h>

#include <atomic>
#include <climits>
#include <string>
#include <cstdint>

namespace wwe2k14::wm30_fire {

inline std::atomic<bool> g_watch{false};           // set when a fire was made, cleared by the tick (post-14 state logged / frame cap / left the match)
inline std::atomic<int32_t> g_last_state{-1};      // the last flow state id the watch saw entered
inline std::atomic<bool> g_post_logged{false};     // the state AFTER 14 / 19 was logged
inline std::atomic<int32_t> g_post_state{-1};      // lane 150: that state's id
inline std::atomic<uint64_t> g_back_frame{0};      // lane 150: the frame state 12 (the running match) was entered after the post state; 0 = not yet
inline std::atomic<int32_t> g_back_clock{INT32_MIN};  // lane 150: the match clock at the first tick from that frame; INT32_MIN = not read
inline std::atomic<bool> g_back_logged{false};     // lane 150: the "back in the match" line was written
inline void ResetBack() { g_post_state.store(-1), g_back_frame.store(0), g_back_clock.store(INT32_MIN), g_back_logged.store(false); }  // lane 150

// What the state after the scene's resume state (14) means (lane 150, from run 7092 and docs/systems/wm30.md "After the moment: flow 14 -> 10"):
// 10 = RESUMED (10 -> 12, the running match; 13 in a demo match); 8 = RESUMED through 9 -> 10; 21 = the MATCH-END path (21 -> 24 -> 4);
// 15 = the G+18576 == 9 branch [U]. 24 and 4 cannot follow 14.
inline const char* PostStateWord(int32_t id) {
  return id == 10 ? "RESUMED (10 -> 12, the running match)"
         : id == 8 ? "RESUMED through 9 -> 10"
         : id == 21 ? "MATCH-END path (21 -> 24 -> 4)"
         : id == 15 ? "the G+18576 == 9 branch [U]"
         : "unclassified";
}

// lane 150: the proof line once state 12 is entered: the clock 60 frames on (C1 != C0 = running).
inline std::string BackLine(uint64_t frame, int32_t c0, int32_t c1) {
  return "fire: back in the match (state 12) at frame " + std::to_string(frame) + ", clock " + std::to_string(c0) + " -> " + std::to_string(c1) +
         " over 60 frames (" + (c1 != c0 ? "running" : "held") + ")";
}
// lane 150: the watch is over when the match is back (line written), or the post state cannot lead back (21, 15, unclassified).
inline bool WatchDone() {
  if (!g_post_logged.load()) return false;
  const int32_t p = g_post_state.load();
  if (p == 10 || p == 8) return g_back_logged.load();
  return true;
}

// sub_827A9AE0 (SetState): the flow replaces state `old_id` by `new_id`.
inline void LogFlowSet(int32_t old_id, int32_t new_id, uint64_t frame) {
  const int32_t prev = g_last_state.exchange(new_id);
  REXLOG_INFO("[wwe2k14][wm30] fire: flow state {} -> {} (frame {}){}", old_id, new_id, frame,
              new_id == 25 ? " [the queued scene's state]" : new_id == 14 ? " [after a flag-0 scene: the 'play the match' tail]" : new_id == 19 ? " [no kind-6 tail: the post-match path]" : "");
  if (old_id == 14 || old_id == 19 || prev == 14 || prev == 19) {
    REXLOG_INFO("[wwe2k14][wm30] fire: after {} the flow goes to state {}: {} (frame {})", old_id == 14 || old_id == 19 ? old_id : prev, new_id, PostStateWord(new_id), frame);
    if (!g_post_logged.load()) g_post_state.store(new_id);  // lane 150
    g_post_logged.store(true);
  } else if (new_id == 12 && g_post_logged.load() && g_back_frame.load() == 0) {
    g_back_frame.store(frame);  // lane 150: the tick reads the clock at this frame and 60 frames on
  }
}

// sub_827BDA68 (the entrance / scene states' "next state" pick): `id` -> `next`, with its inputs.
inline void LogFlowNext(int32_t id, int32_t next, int32_t ent_9644, uint32_t g468, uint32_t g18576, uint64_t frame) {
  REXLOG_INFO("[wwe2k14][wm30] fire: flow state {} picks next {} (R+9644={} G+468={} G+18576={}) frame {}", id, next, ent_9644, g468, g18576, frame);
}

}  // namespace wwe2k14::wm30_fire
