// lane 185 (Session 101): the DLSS camera-jitter window, in one place for the live reader (reader.h ApplyJitter) and the offline replay (rplay --jitter,
// jitter_replay.h). No game or SDK includes. Design and numbers: docs\renderer\WIDE_OURS.md section 7.
//
// DLSS's colour input is everything up to and including the post chain's last pass (the composite marker, vertex shader patched hash D6B8DEB0775CC47F);
// the draws after it (the HUD, the 30 Years filter) are not in DLSS's input and stay unjittered. DLSS removes the frame's jitter from the whole input, so every
// scene draw up to the marker has to be moved by it. The S10b rule ended the window at the frame's first scene-size depth copy (the SDK's DLAA rule copied across), so
// a two-layer depth-of-field entrance's layer-2 wrestlers / props and every particle (drawn after that copy) kept no jitter: moved by -jitter in DLSS's history
// (edge wobble, history rejection). `legacy` keeps the S10b rule (diagnosis switch --wwe2k14_rdr_dlss_jitter_legacy, the offline plant).
#pragma once
#include <cstdint>

namespace rdr_adapter::jitter {

inline constexpr uint64_t kDefaultMarker = 0xD6B8DEB0775CC47FULL;  // the SDK's gpu_dlss_hud_after_vs default (patched hash)

struct Window {
  bool hud = false;     // the composite marker has been drawn this frame
  bool legacy = false;  // the S10b rule: the window ends at the first scene depth copy
  // Armed: the PREVIOUS frame had its marker. DLSS runs only on a frame that has the marker (the binder arms its colour copy there), so a frame after one without it (a
  // menu, pause, replay, photo mode) is not jittered: nothing would remove the jitter, and its HUD-phase draws (drawn after the first depth copy) would shimmer.
  // The cost: the first game frame after a menu is unjittered (DLSS resets there anyway). The legacy window ignores it (it is the old rule).
  bool armed = true;
};

// Is the jitter window open? `scene_done`: the frame's first scene-size depth copy has happened.
inline bool Open(const Window& w, bool scene_done) { return w.legacy ? !scene_done : !w.hud; }

// The draw facts the rule looks at.
struct DrawFacts {
  bool rect = false;            // a Rect primitive
  bool vte_off = false;         // the viewport transform is off (pixel-placed)
  bool clip_off = false;        // kDynClipOff (pixel-placed, the offline replay's twin of vte_off)
  bool empty_viewport = false;  // kDynEmptyViewport
  bool full_view = false;       // a flat rectangle whose corners reach the view's four edges (a screen-space composite / copy / fade, wide_rule.h FullViewQuad): not part of the 3D scene, never moved
  float vw = 0, vh = 0;         // the host viewport (guest pixels)
  uint64_t vs_hash = 0;         // the vertex shader's patched hash
};

// Does the draw get this frame's jitter? Notes the marker (the draw itself is classified first, as wide_rule.h Decide does).
inline bool Moves(Window& w, bool scene_done, const DrawFacts& d, uint64_t marker = kDefaultMarker) {
  if (!Open(w, scene_done)) return false;
  const bool moved = (w.legacy || w.armed) && !(d.vw < 1200.0f || d.vh <= 0 || d.rect || d.vte_off || d.clip_off || d.empty_viewport || d.full_view);
  if (marker && d.vs_hash == marker) w.hud = true;
  return moved;
}

// The next frame (the present).
inline void NextFrame(Window& w) {
  w.armed = w.hud;
  w.hud = false;
}

}  // namespace rdr_adapter::jitter
