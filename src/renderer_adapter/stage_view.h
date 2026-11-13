// S101 lane 59 (Check E, run 7066): the STAGE VIEW. `!scene play ID` from the main menu plays the scene, but into the Create a Story preview's own scene target
// (a SceneColor surface the editor never shows in mode 2); the presented picture is the editor's red background. While a stage push is playing, the owner
// renderer presents the scene's target instead. We own presentation: nothing here reproduces what the editor would have done.
//
// Which target: by declared surface role (names.h SurfaceRole::SceneColor with kRoleDeclared, set where the game creates the surface: surface_role_table.h
// 0x82C22AF8 "scene targets"), never by size or index. A frame can hold several such surfaces (the editor keeps three, run 7066 frame 1979: 693, 1026, 1074); the
// scene is the one the frame DREW INTO, last (the characters' passes). The background's own picture (image 8) has no declared role. The binder tracks it per
// frame (SceneTracker); at the present it copies that target over the presented surface (aspect kept, the rest of the surface is left as it is).
//
// The director (scene_director.h) says "playing" through one atomic here; the renderer thread reads it. No game or SDK includes.
#pragma once
#include <atomic>
#include <cstdint>

#include "names.h"

namespace rdr_adapter::stage_view {

inline bool g_enabled = false;                // --wwe2k14_stage_view (default OFF, new things are off by default; effect only for frames stamped "playing" while a `!scene play` is active)
inline std::atomic<bool> g_playing{false};    // the director: a stage push is dispatched and not finished. Read on the GUEST thread when a frame is captured and stamped into
                                              // its header (kFrameStagePlaying); the binder reads the stamp, never this.
inline bool g_force = false;                  // fview only (--stage-view): every frame counts as stamped (captures made before the stamp existed)
inline int g_pick_fault = 0;                  // fview only (--stage-pick-fault last_by_id|first_by_id): planted twins of the target pick, 1 = the highest-id declared SceneColor, 2 = the lowest-id one

inline void SetPlaying(bool playing) { g_playing.store(playing, std::memory_order_relaxed); }
// Guest thread, at capture: should this frame be stamped?
inline bool StampFrame() { return g_enabled && g_playing.load(std::memory_order_relaxed); }
// Render thread / fview, at bind: is the stage view on for a frame whose header flags are `frame_flags` (kFrameStagePlaying = 16)?
inline bool FrameOn(uint32_t frame_flags) { return g_enabled && (g_force || (frame_flags & 16u) != 0); }

// The rule for "this image record is a scene target": its creator declared it SceneColor (not a guess by shape, not the unnamed screen-size pair).
inline bool IsSceneTarget(uint8_t role, uint8_t role_flags) {
  return role == uint8_t(SurfaceRole::SceneColor) && (role_flags & kRoleDeclared) != 0;
}

// Per frame: the scene-role colour target the frame drew into last.
struct SceneTracker {
  uint32_t image = 0;   // the capture's image id (0 = none this frame)
  uint32_t draws = 0;   // draws into it
  void Reset() { image = 0, draws = 0; }
  // `color_image`: the colour target[0] bound for this draw; `is_scene`: IsSceneTarget of its record.
  void NoteDraw(uint32_t color_image, bool is_scene) {
    if (!is_scene || !color_image) return;
    if (color_image != image) image = color_image, draws = 0;
    ++draws;
  }
};

struct FitRect { int32_t x, y; uint32_t w, h; };

// The largest rectangle of the source's aspect inside the destination, centred (the destination's size when the aspects are equal).
inline FitRect Fit(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h) {
  if (!src_w || !src_h || !dst_w || !dst_h) return {0, 0, dst_w, dst_h};
  if (uint64_t(src_w) * dst_h == uint64_t(dst_w) * src_h) return {0, 0, dst_w, dst_h};
  if (uint64_t(src_w) * dst_h > uint64_t(dst_w) * src_h) {  // the source is wider: bars above and below
    const uint32_t h = uint32_t(uint64_t(dst_w) * src_h / src_w);
    return {0, int32_t((dst_h - h) / 2), dst_w, h};
  }
  const uint32_t w = uint32_t(uint64_t(dst_h) * src_w / src_h);
  return {int32_t((dst_w - w) / 2), 0, w, dst_h};
}

}  // namespace rdr_adapter::stage_view
