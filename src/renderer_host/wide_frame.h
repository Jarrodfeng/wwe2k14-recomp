// S105 A2 (docs\research\s105\A2.md): the stream-free host's ultrawide / tall-screen frame shape, in one pure header (no game, SDK or GPU includes; tools\renderer\wide_frame_unit.cpp tests it).
//
// The game's frame is 1280x720. During play (sequence word 9, ultrawide.h FillNow) on a screen wider than 16:9 the host makes the scene and the final image F
// round(1280 x k) x 720 (k = gpu_wide_factor = the screen's aspect / (16/9)); on a taller screen 1280 x round(720 x t) (t = gpu_tall_factor = (16/9) / aspect). The camera
// is a view plus the game's vertical FOV, so rf::BuildFrame (record_frame.cpp) shows more at the sides (Hor+), or more above and below (Vert+: the game's horizontal view
// is kept, `ys x min(1, aspect / (16/9))`). The picture is never stretched (present_fill stays false): the present fits F to the window, whose shape F now has.
// The 2D rows stay in the game's 1280x720 pixels: they are drawn into the centred 1280x720 box of F; a full-view flat rectangle (a fade / flash / tint) takes the whole F.
// Outside play (menus, Create modes) the frame is the game's own 16:9 (fill_now = false), as before. At 16:9 (wide = tall = 1) every function here is the identity.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../renderer_adapter/wide_rule.h"
#include "../renderer_scene/rows_quad2d.h"

namespace wwe2k14::scene_host::wide_frame {

struct FrameShape {
  uint32_t w = 0, h = 0;          // the scene / F size in guest pixels (the game's own size when not widened)
  uint32_t game_w = 0, game_h = 0;   // the game's frame (the 2D rows' pixel space)
  int32_t box_x = 0, box_y = 0;   // the centred game box's top-left inside w x h
  bool widened = false;           // w x h differs from the game's size
};

// The frame for a game frame of game_w x game_h. wide / tall are the factors (1 = the game's shape; only one of them is above 1). `fill_now` = the in-game sequence.
inline FrameShape Shape(uint32_t game_w, uint32_t game_h, float wide, float tall, bool fill_now, bool twin_box_origin = false) {
  FrameShape s;
  s.w = s.game_w = game_w, s.h = s.game_h = game_h;
  if (!fill_now || !game_w || !game_h) return s;
  // (review F5) rounded to the nearest EVEN size: the half- / quarter-resolution post and shadow passes never see an odd width, and the 2D box is then exactly centred.
  if (wide > 1.001f) s.w = uint32_t(2 * std::lround(double(game_w) * double(wide) / 2.0));
  else if (tall > 1.001f) s.h = uint32_t(2 * std::lround(double(game_h) * double(tall) / 2.0));
  if (s.w < game_w) s.w = game_w;
  if (s.h < game_h) s.h = game_h;
  s.widened = s.w != game_w || s.h != game_h;
  if (!twin_box_origin) s.box_x = int32_t((s.w - game_w) / 2), s.box_y = int32_t((s.h - game_h) / 2);   // (planted twin: the box left at the origin)
  return s;
}

// The projection rule of rf::BuildFrame for a target aspect (record_frame.cpp): the y scale factor on cot(fov / 2). 1 at 16:9 and wider (Hor+ keeps the vertical FOV); a taller target keeps the
// game's horizontal view (Vert+). `twin_off` = the planted twin (no tall rule).
inline float TallScale(float aspect, bool twin_off = false) { return twin_off ? 1.f : (aspect / (16.f / 9.f) < 1.f ? aspect / (16.f / 9.f) : 1.f); }

// A row that is a full-view flat rectangle (a fade / flash / tint, shape only): it covers the whole target, not the centred box. Wraps rdr_adapter::wide::FullViewQuad (lane 177).
inline bool FullViewRow(const rdr_scene::Quad2D& r) {
  if (r.kind != rdr_scene::kQkRect) return false;
  if ((r.have & (rdr_scene::kQhVerts | rdr_scene::kQhPosScale)) != (rdr_scene::kQhVerts | rdr_scene::kQhPosScale)) return false;
  rdr_adapter::wide::FlatQuad q;
  for (int i = 0; i < 4; ++i) q.x[i] = r.verts[i][0], q.y[i] = r.verts[i][1];
  q.pos_x = r.pos_scale[0], q.pos_y = r.pos_scale[1], q.use_proj = r.use_proj;
  return rdr_adapter::wide::FullViewQuad(q);
}

// The viewport and scissor of one 2D row on a target of tgt_w x tgt_h that holds the game's box (game_w x game_h at origin_x, origin_y). `have_sc` / sc = the row's own scissor already offset by the origin (RowScissor).
// A full-view flat row (a fade / flash / tint) takes the whole target, but only when the game gave it no scissor or a scissor of the whole game frame (review F2): a tint the game clips to a panel stays in that panel.
struct IRect {
  int32_t x = 0, y = 0;
  uint32_t w = 0, h = 0;
  bool operator==(const IRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};
struct RowView {
  IRect viewport, scissor;
  bool full_target = false;
};
inline RowView RowViewOf(uint32_t game_w, uint32_t game_h, uint32_t tgt_w, uint32_t tgt_h, int32_t origin_x, int32_t origin_y, bool full_view_row, bool have_sc, const IRect& sc, bool twin_scissor_ignored = false) {
  RowView v;
  const IRect game_box{origin_x, origin_y, game_w, game_h};
  v.full_target = (tgt_w != game_w || tgt_h != game_h) && full_view_row && (twin_scissor_ignored || !have_sc || sc == game_box);
  v.viewport = v.full_target ? IRect{0, 0, tgt_w, tgt_h} : game_box;
  v.scissor = have_sc && !v.full_target ? sc : IRect{0, 0, tgt_w, tgt_h};
  return v;
}

// The game's box inside a scene colour of scene_w x scene_h whose frame shape is shape_w x shape_h (the in-scene 2D rows: scene_host.cpp SetScene2D): the same fractions of the scene as the game's box is of the frame, centred.
inline IRect SceneBoxOf(uint32_t scene_w, uint32_t scene_h, uint32_t game_w, uint32_t game_h, uint32_t shape_w, uint32_t shape_h) {
  IRect b{0, 0, scene_w, scene_h};
  if (!shape_w || !shape_h || !game_w || !game_h || (shape_w == game_w && shape_h == game_h)) return b;
  b.w = uint32_t((uint64_t(scene_w) * game_w + shape_w / 2) / shape_w), b.h = uint32_t((uint64_t(scene_h) * game_h + shape_h / 2) / shape_h);
  b.x = int32_t((scene_w - b.w) / 2), b.y = int32_t((scene_h - b.h) / 2);
  return b;
}

// The centred box of the game's shape (game_w : game_h) inside a w x h picture: the picture whole when its shape already is the game's (within 16 / (w h) rounding), else the full height and the game's
// width (a wider picture, Hor+) or the full width and the game's height (a taller one, Vert+). Used for the big screen's grab and the guest read-backs (the game's textures are 16:9).
struct Box {
  uint32_t x = 0, y = 0, w = 0, h = 0;
};
inline Box BoxOf(uint32_t w, uint32_t h, uint32_t game_w = 1280, uint32_t game_h = 720) {
  Box b{0, 0, w, h};
  if (!w || !h || !game_w || !game_h) return b;
  const uint64_t lhs = uint64_t(w) * game_h, rhs = uint64_t(h) * game_w, slack = 16ull * game_h / 9;
  if (lhs > rhs + slack) {   // wider
    b.w = uint32_t((uint64_t(h) * game_w + game_h / 2) / game_h), b.x = (w - b.w) / 2;
  } else if (rhs > lhs + slack) {   // taller
    b.h = uint32_t((uint64_t(w) * game_h + game_w / 2) / game_w), b.y = (h - b.h) / 2;
  }
  return b;
}

// The centred game box cut out of a w x h picture of `bpp` bytes a pixel (the guest read-backs). Sets cw / ch; the bytes are the whole picture when nothing is cropped.
inline std::vector<uint8_t> CropBox(const uint8_t* src, uint32_t w, uint32_t h, uint32_t bpp, uint32_t game_w, uint32_t game_h, uint32_t* cw, uint32_t* ch) {
  std::vector<uint8_t> out;
  if (!src || !w || !h) return out;
  const Box b = BoxOf(w, h, game_w, game_h);
  out.resize(size_t(b.w) * b.h * bpp);
  for (uint32_t y = 0; y < b.h; ++y) std::memcpy(out.data() + size_t(y) * b.w * bpp, src + (size_t(b.y + y) * w + b.x) * bpp, size_t(b.w) * bpp);
  if (cw) *cw = b.w;
  if (ch) *ch = b.h;
  return out;
}

}  // namespace wwe2k14::scene_host::wide_frame
