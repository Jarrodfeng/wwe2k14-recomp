// lane 171 (Session 101, release gate): the screen-shape (ultrawide / taller screens) squeeze rule, in one place for the live reader and
// the offline replay (rplay --wide, wide_replay.h). No game or SDK includes.
//
// What the rule is (the old path's anamorphic Hor+, SDK "Session 33, WIDE"; moved into the reader by settings parity S5): the game renders its
// 16:9 frame; the scene's geometry draws see clip x / wide (y / tall) so the same frame holds a wider view; the present stretches the frame to
// the window (present_fill). The HUD draws, after the post chain's last pass, are squeezed the same way, so after the stretch they are a
// centred 16:9. Screen-space (pixel-placed) draws and the post chain's own passes are left alone: they work on the squeezed frame.
//
// Which draws are "the scene's" (lane 171, the Undertaker's blocks, GATE_FAULTS row 3): a draw that transforms by a projection (not a Rect, not
// pixel-placed), into a scene-sized viewport (>= min_width guest pixels), BEFORE the post chain's last pass. The S5 version also required "before
// the frame's first scene depth copy" (the SDK's DLAA jitter rule, copied across). A two-layer depth-of-field frame (Bryan's / the Undertaker's
// entrance, docs\systems\post_effects.md section 3) copies the scene's depth for the far blur FIRST and draws the props and both wrestlers
// AFTER it, so the wrestlers were drawn unsqueezed while the background they sit in was squeezed: the foreground 1/wide too wide against its
// own depth-of-field layers. `legacy` keeps the S5 rule (diagnosis switch --wwe2k14_rdr_wide_legacy, the offline plant).
//
// lane 177 (Session 101): a FULL-VIEW quad (a fade, a flash, a tint: a flat rectangle whose four corners reach the 16:9 view's four edges) is not squeezed.
// Squeezed, it would cover only the middle 1/k of a wider screen and leave the sides unfaded / unflashed. Unsqueezed it covers the whole 1280 x 720 frame, and
// the present stretches the frame to the window, so it covers the whole displayed width (the 30 Years filter's quad is left alone for the same reason, by hash).
// It is found by SHAPE (FullViewQuad), not by a hash list. `no_full_view` keeps the lane 171 behaviour (diagnosis --wwe2k14_rdr_wide_no_fullview, the offline plant).
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace rdr_adapter::wide {

struct State {
  float wide = 1, tall = 1;
  uint32_t min_width = 1200;
  uint64_t hud_after_vs = 0;
  std::vector<uint64_t> post_vs;
  bool scene_done = false, hud = false, menu = false, all = false;
  bool hud_seen = false, post_seen = false;  // logged once each
  bool legacy = false;                       // lane 171: the S5 rule (the scene ends at the first scene depth copy)
  bool no_full_view = false;                 // lane 177: the plant / diagnosis: full-view quads are squeezed like any scene draw
  bool full_seen = false;                    // logged once
  bool offscreen_squeezed = false;           // lane 244b: the plant (rplay --wide-offscreen-squeezed): offscreen passes are squeezed in the blanket modes (the ghost outline of the CAW editor)
  bool clip_off_squeezed = false;            // lane 244: the plant (rplay --wide-clipoff-squeezed): clip-off draws are squeezed like any other (the 976 px shift)
  bool on() const { return wide > 1.001f || tall > 1.001f; }
};

struct Verdict {
  bool squeeze = false;
  bool post_after = false;        // a full-screen pass after the HUD marker that stays unsqueezed (the 30 Years filter)
  bool post_after_first = false;  // the first time (log once)
  bool marker = false;            // this draw is the post chain's last pass (the HUD follows it)
  bool marker_first = false;
  bool full_view = false;         // lane 177: a full-view quad, left unsqueezed so it covers the whole displayed width
  bool full_view_first = false;   // the first time (log once)
  bool offscreen = false;         // lane 244b: a clip-on draw into a target narrower than min_width (an offscreen pass: shadow / rim / outline / blur buffer) the blanket modes would have squeezed: left alone
  bool pixel_placed = false;      // lane 244: a clip-off (pixel-placed) draw the blanket modes (menu frame, gpu_wide_all, HUD phase) would have squeezed: left alone
};

// lane 177: the facts of a flat pixel-placed rectangle (the game's T_Rect: four (x, y, z) vertices, PosScale c0 = (2 / view width, 2 / view height),
// fUseProjMatrix c28.x = 0; clip = ((x, y) - 0.5) * PosScale - 1, so pixel x in [0.5, W + 0.5] spans the view; UI_QUADS_OURS.md section 1).
struct FlatQuad {
  float x[4] = {}, y[4] = {};
  float pos_x = 0, pos_y = 0;  // PosScale.xy
  float use_proj = 0;          // fUseProjMatrix
};

// Does the rectangle cover the whole view (all four edges, within `slack` guest pixels)? A degenerate quad (a line, a point) and a rotated one do not. No hash:
// the corners decide. The view size is the constants' own (2 / PosScale), 1280 x 720 for the game.
inline bool FullViewQuad(const FlatQuad& q, float slack = 1.0f) {
  if (!(q.pos_x > 0) || !(q.pos_y > 0) || q.use_proj != 0) return false;
  const float w = 2.0f / q.pos_x, h = 2.0f / q.pos_y;
  if (!(w >= 320.0f && w <= 16384.0f && h >= 180.0f && h <= 16384.0f)) return false;
  float x0 = q.x[0], x1 = q.x[0], y0 = q.y[0], y1 = q.y[0];
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(q.x[i]) || !std::isfinite(q.y[i])) return false;
    x0 = q.x[i] < x0 ? q.x[i] : x0, x1 = q.x[i] > x1 ? q.x[i] : x1;
    y0 = q.y[i] < y0 ? q.y[i] : y0, y1 = q.y[i] > y1 ? q.y[i] : y1;
  }
  if (!(x1 - x0 > 0.5f * w) || !(y1 - y0 > 0.5f * h)) return false;  // a real area (not a line)
  bool corner[4] = {false, false, false, false};                     // axis-aligned: every vertex sits on a corner of the box, all four corners used
  for (int i = 0; i < 4; ++i) {
    const bool lx = std::fabs(q.x[i] - x0) < 0.01f, hx = std::fabs(q.x[i] - x1) < 0.01f;
    const bool ly = std::fabs(q.y[i] - y0) < 0.01f, hy = std::fabs(q.y[i] - y1) < 0.01f;
    if (!(lx || hx) || !(ly || hy)) return false;
    corner[(hx ? 1 : 0) + (hy ? 2 : 0)] = true;
  }
  if (!(corner[0] && corner[1] && corner[2] && corner[3])) return false;
  return x0 <= 0.5f + slack && x1 >= w + 0.5f - slack && y0 <= 0.5f + slack && y1 >= h + 0.5f - slack;
}

// lane 244 (Session 101, the CAW editor in a 21:9 / menu frame): a CLIP-OFF draw (kDynClipOff: vertices already in guest pixels, drawn into the 8192 x 8192 pixel-space
// viewport) is never squeezed. Two reasons, both measured (docs/renderer/POST_OURS.md, lane 244): (1) the squeeze divides ndc_offset by `wide`, which is right for a viewport
// whose centre is the frame's centre, but a clip-off draw's NDC origin is the 8192 viewport's centre (x = 4096), not the frame's (x = 640): the pass landed 976 guest pixels
// to the right (the CAW editor's post combine drew only a 304 px strip, the wrestler vanished; the small blur / downsample passes landed outside their own scissors and drew nothing).
// (2) what a clip-off draw is: the post chain's own passes and offscreen passes (blur, downsample): they work in their own pixel space on the frame that is ALREADY squeezed (this
// header's first paragraph), so squeezing them again is wrong even when placed. Matches never met this (their clip-off draws: 0 of 1377 squeezed on s101_sweep_match_type); only
// the blanket modes (a menu frame, gpu_wide_all, the HUD phase) squeezed them. Rule 6: the 360's single 8192 pixel-space viewport made the clip-off NDC origin not the frame centre;
// we do not need that detail, so these draws are simply left alone.
// Decides one draw. `screen_space`: a Rect primitive or a pixel-placed draw (VTE off). `viewport_w`: the guest-pixel width of the draw's host
// viewport (8192 for a pixel-placed draw). `vs_sdk_hash`: the vertex shader's patched hash (the SDK's own hashes). Updates `g` (the marker).
inline Verdict Decide(State& g, bool screen_space, float viewport_w, uint64_t vs_sdk_hash, bool full_view_quad = false, bool clip_off = false) {
  Verdict v;
  const bool scene_window = g.legacy ? !g.scene_done : !g.hud;  // lane 171
  const bool scene = scene_window && viewport_w >= float(g.min_width);
  if (g.hud)
    for (const uint64_t h : g.post_vs) v.post_after |= h == vs_sdk_hash;
  if (v.post_after && !g.post_seen) {
    g.post_seen = true;
    v.post_after_first = true;
  }
  v.squeeze = (scene && !screen_space) || (g.hud && !v.post_after) || g.menu || g.all;
  if (v.squeeze && clip_off && !g.clip_off_squeezed) {  // lane 244 (g.clip_off_squeezed = the plant: the lane 171 .. 243 behaviour)
    v.squeeze = false;
    v.pixel_placed = true;
  }
  // lane 244b (S101, live check 7169: a dark ghost outline offset ~15 px left of the CAW wrestler): a pass that renders into an OFFSCREEN target (viewport narrower than min_width: the
  // wrestler's own 512 x 512 / 512 x 256 buffers) is never squeezed. Those buffers are read back by the frame-sized draws through their own projection; squeezing the content in the
  // buffer while the readers are (or are not) squeezed makes the two layers disagree by the squeeze. Only the draws into the frame itself (viewport >= min_width) are the on-screen ones.
  // (The scene rule already needed viewport_w >= min_width; the blanket modes - menu frame, gpu_wide_all, HUD phase - did not ask.)
  if (v.squeeze && !clip_off && viewport_w < float(g.min_width) && !g.offscreen_squeezed) {
    v.squeeze = false;
    v.offscreen = true;
  }
  if (v.squeeze && full_view_quad && !g.no_full_view && viewport_w >= float(g.min_width)) {  // (a frame-sized viewport only: an offscreen pass keeps whatever it had)  // lane 177: a fade / flash / tint covers the whole frame, so the whole displayed width
    v.squeeze = false;
    v.full_view = true;
    if (!g.full_seen) {
      g.full_seen = true;
      v.full_view_first = true;
    }
  }
  if (vs_sdk_hash == g.hud_after_vs && !g.hud) {
    g.hud = true;
    v.marker = true;
    if (!g.hud_seen) {
      g.hud_seen = true;
      v.marker_first = true;
    }
  }
  return v;
}

// The next frame's markers (a frame after one without a scene depth copy is a menu). `all_flag` = gpu_wide_all.
inline void NextFrame(State& g, bool all_flag) {
  g.menu = !g.scene_done;
  g.scene_done = g.hud = false;
  g.all = all_flag;
}

}  // namespace rdr_adapter::wide
