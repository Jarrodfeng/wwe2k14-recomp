// S103 RBC fix2: the pure rules of SceneHost's scene read-backs (RequestScene / CaptureScene), so a unit can prove them (tools\readback\rbc_unit.cpp builds THIS file; no GPU, no game).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// F1 (stale scene): `out` is a scene only while it is the picture of a RECENT OURS 3D frame. A menu, a loading screen or a replay the game draws itself leaves the last match's `out` in place, which must never be
//    handed to a read-back (it would be saved into the player's custom arena / clip).
// F2 (shot vs scene key): the renderer allows ONE queued read-back per image. A screenshot has priority: the scene callers wait while a screenshot copy is in flight, a refused scene copy goes back for a few
//    frames, and a screenshot refused while a scene copy is in flight WAITS (it never fails because a scene copy holds the key).
#pragma once

#include <cstdint>

namespace wwe2k14::scene_host::scene_rules {

#ifdef RBC_PLANTS
// Test only: 7 = Fresh ignores the frame age; 8 = a refused scene copy is never retried; 9 = a screenshot refused while a scene copy is in flight fails.
inline int g_plant = 0;
#define SCENE_RULE_PLANT(n) (::wwe2k14::scene_host::scene_rules::g_plant == (n))
#else
#define SCENE_RULE_PLANT(n) false
#endif

inline constexpr uint64_t kMaxAge = 2;   // guest frames between the frame `out` was drawn for and the frame that asks
inline constexpr int kRetries = 3;       // a refused scene copy is retried this many frames

// `out` complete (the state's last3d_valid) and held (out_ok), an image exists, drawn within kMaxAge frames of `frame` (a frame before it is not fresh either).
inline bool Fresh(bool out_ok, bool last3d_valid, bool has_image, uint64_t frame, uint64_t out_frame) {
  if (!out_ok || !last3d_valid || !has_image) return false;
  if (SCENE_RULE_PLANT(7)) return true;
  return frame >= out_frame && frame - out_frame <= kMaxAge;
}

enum class Step : uint8_t { Wait, NoScene, Queue };
// What QueueScene does with a waiting batch.
inline Step Decide(bool shot_inflight, bool fresh) {
  if (shot_inflight) return Step::Wait;
  return fresh ? Step::Queue : Step::NoScene;
}

// The renderer refused the scene copy: go back for another frame (true) or give the callers their answer (false). `retries` counts this batch's refusals.
inline bool Retry(int& retries) {
  if (SCENE_RULE_PLANT(8)) return false;
  return ++retries <= kRetries;
}

// A screenshot copy was refused: true = it waits for the next frame (a scene copy holds the image's key), false = a real refusal (answered not-ok).
inline bool ShotRefusalIsWait(bool scene_inflight, bool is_8bit) {
  if (SCENE_RULE_PLANT(9)) return false;
  return scene_inflight && is_8bit;
}

}  // namespace wwe2k14::scene_host::scene_rules
