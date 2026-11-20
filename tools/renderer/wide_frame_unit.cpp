// S105 A2: the offline proof of src\renderer_host\wide_frame.h (the stream-free host's ultrawide / tall-screen frame shape: the size, the centred 1280x720 box, the projection's tall rule, the
// full-view row test, the box crop). 0 game runs, no GPU, no SDK. Built and run by tools\renderer\wide_frame_unit.py.
//   wide_frame_unit            all arms (exit 0 when every check passed)
//   wide_frame_unit plant N    the same with planted fault N (it must exit 1 with a FAIL line):
//     1 box_origin        the 2D box stays at the frame's origin (the HUD hugs the left)        -> "box.centred"
//     2 tall_rule_off     the projection keeps the vertical FOV on a tall target (no Vert+)       -> "tall.horizontal_kept"
//     3 menu_widens       a menu frame (fill_now false) is widened too                            -> "menu.game_shape"
//     4 full_view_off     a full-view fade is not told from a HUD row                              -> "fullview.fade"
//     5 scissor_ignored   a full-view tint the game clips to a panel widens to the whole target   -> "rowview.panel_tint_boxed"
//     6 origin_dropped    the 2D row's viewport is at the target's origin, not the box's           -> "rowview.box_origin"
//     7 scissor_dropped   a HUD row's own scissor is not applied                                   -> "rowview.hud_scissor"
//     8 scene_box_origin  the in-scene box is not centred in the scene colour                      -> "scenebox.centred"
#include "../../src/renderer_host/wide_frame.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wf = wwe2k14::scene_host::wide_frame;
namespace q = rdr_scene;

static int g_plant = 0, g_fail = 0, g_ok = 0;
static void Check(bool ok, const char* name, const std::string& detail = "") {
  if (ok) {
    ++g_ok;
    std::printf("ok   %s %s\n", name, detail.c_str());
  } else {
    ++g_fail;
    std::printf("FAIL %s %s\n", name, detail.c_str());
  }
}

// The screen shape's factors exactly as ultrawide::Configure makes them (k = aspect / (16 / 9) clamped 1..4, t = (16 / 9) / aspect clamped 1..4): only one of them is above 1.
static void Factors(double aspect, float& k, float& t) {
  const double kk = std::fmin(std::fmax(aspect / (16.0 / 9.0), 1.0), 4.0), tt = std::fmin(std::fmax((16.0 / 9.0) / aspect, 1.0), 4.0);
  k = kk >= 1.01 ? float(kk) : 1.f;
  t = kk < 1.01 && tt >= 1.01 ? float(tt) : 1.f;
}

static q::Quad2D Rect(float x0, float y0, float x1, float y1) {
  q::Quad2D r;
  r.kind = q::kQkRect;
  r.have = q::kQhVerts | q::kQhPosScale;
  r.pos_scale[0] = 2.f / 1280.f, r.pos_scale[1] = 2.f / 720.f;
  const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
  for (int i = 0; i < 4; ++i) r.verts[i][0] = xs[i], r.verts[i][1] = ys[i];
  return r;
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "plant") g_plant = std::atoi(argv[2]);

  // ---- 16:9 is the game's frame, byte for byte ----
  {
    const wf::FrameShape s = wf::Shape(1280, 720, 1.f, 1.f, true);
    Check(s.w == 1280 && s.h == 720 && s.box_x == 0 && s.box_y == 0 && !s.widened, "shape.identity_16_9", "1280x720");
    const wf::FrameShape s2 = wf::Shape(1920, 1080, 1.f, 1.f, true);
    Check(s2.w == 1920 && s2.h == 1080 && !s2.widened, "shape.identity_1080");
    Check(wf::TallScale(1280.f / 720.f, g_plant == 2) == 1.f && wf::TallScale(16.f / 9.f) == 1.f && wf::TallScale(1920.f / 1080.f) == 1.f, "tall.identity_16_9");
    const wf::Box b = wf::BoxOf(1280, 720);
    Check(b.x == 0 && b.y == 0 && b.w == 1280 && b.h == 720, "box.identity_16_9");
    const wf::Box b2 = wf::BoxOf(1366, 768);   // a 1366x768 window is 16:9 within a pixel: the whole picture
    Check(b2.w == 1366 && b2.h == 768, "box.identity_1366");
  }

  // ---- wider screens: the scene and F are round(1280 k) x 720; the 2D box is the centred 1280x720 ----
  struct Wide { const char* name; double aspect; uint32_t w; } wides[] = {
      {"21:9 1680x720", 21.0 / 9.0, 1680}, {"2560x1080", 2560.0 / 1080.0, 1706}, {"3440x1440", 3440.0 / 1440.0, 1720}, {"32:9", 32.0 / 9.0, 2560}, {"3840x1080", 3840.0 / 1080.0, 2560}};
  for (const Wide& wd : wides) {
    float k, t;
    Factors(wd.aspect, k, t);
    wf::FrameShape s = wf::Shape(1280, 720, k, t, true, g_plant == 1);
    Check(s.w == wd.w && s.h == 720 && s.widened, "shape.wide", std::string(wd.name) + " -> " + std::to_string(s.w) + "x" + std::to_string(s.h));
    Check(std::fabs(double(s.w) / double(s.h) - wd.aspect) < 0.002, "shape.wide_aspect", std::string(wd.name) + " aspect " + std::to_string(double(s.w) / double(s.h)));
    Check(s.box_x == int32_t((s.w - 1280) / 2) && s.box_y == 0 && uint32_t(s.box_x) * 2 + 1280 >= s.w - 1 && uint32_t(s.box_x) * 2 + 1280 <= s.w, "box.centred", std::string(wd.name) + " x0 " + std::to_string(s.box_x));
    // the 2D layer: a HUD pixel x of the game's 1280 lands at box_x + x of the frame; the left / right edge of the 1280 box is symmetric about the frame's centre
    const double left = double(s.box_x), right = double(s.w) - (double(s.box_x) + 1280.0);
    Check(std::fabs(left - right) <= 1.0, "box.symmetric", std::string(wd.name) + " left " + std::to_string(left) + " right " + std::to_string(right));
    // a full-screen fade spans the whole width, not the box: its viewport is the whole target (quads2d_draw.cpp), here the row is told apart by shape
    const q::Quad2D fade = Rect(0.f, 0.f, 1280.f, 720.f);
    const bool is_full = g_plant == 4 ? false : wf::FullViewRow(fade);
    Check(is_full, "fullview.fade", wd.name);
    // the crop to the game's 16:9 box
    const wf::Box cb = wf::BoxOf(s.w, s.h);
    Check(cb.w == 1280 && cb.h == 720 && cb.x == uint32_t(s.box_x), "box.crop_16_9", std::string(wd.name) + " x " + std::to_string(cb.x));
    // a menu frame (not in play) is the game's own 16:9
    wf::FrameShape m = wf::Shape(1280, 720, k, t, g_plant == 3 ? true : false);
    Check(m.w == 1280 && m.h == 720 && !m.widened, "menu.game_shape", wd.name);
  }

  // ---- taller screens: 1280 x round(720 t); the horizontal view of the game is kept ----
  struct Tall { const char* name; double aspect; uint32_t h; } talls[] = {{"16:10", 1.6, 800}, {"4:3", 4.0 / 3.0, 960}, {"3:2", 1.5, 854}};
  for (const Tall& tl : talls) {
    float k, t;
    Factors(tl.aspect, k, t);
    wf::FrameShape s = wf::Shape(1280, 720, k, t, true, g_plant == 1);
    Check(s.h == tl.h && s.w == 1280 && s.widened, "shape.tall", std::string(tl.name) + " -> " + std::to_string(s.w) + "x" + std::to_string(s.h));
    Check(std::fabs(double(s.w) / double(s.h) - tl.aspect) < 0.003, "shape.tall_aspect", tl.name);
    Check(s.box_x == 0 && s.box_y == int32_t((s.h - 720) / 2), "box.centred", std::string(tl.name) + " y0 " + std::to_string(s.box_y));
    // BuildFrame's tall rule keeps the game's HORIZONTAL view: xs = ys x TallScale / aspect equals the 16:9 frame's xs = ys / (16/9)
    const float aspect = float(s.w) / float(s.h);
    const float xs_tall = wf::TallScale(aspect, g_plant == 2) / aspect, xs_game = 1.f / (16.f / 9.f);
    Check(std::fabs(xs_tall - xs_game) < 1e-3f, "tall.horizontal_kept", std::string(tl.name) + " xs " + std::to_string(xs_tall) + " vs " + std::to_string(xs_game));
    const wf::Box cb = wf::BoxOf(s.w, s.h);
    Check(cb.w == 1280 && cb.h == 720 && cb.y == uint32_t(s.box_y), "box.crop_16_9", std::string(tl.name));
  }
  // Hor+ keeps the vertical FOV: ys is untouched at a wide aspect (TallScale 1), so xs = ys / aspect shows more at the sides
  Check(wf::TallScale(2560.f / 1080.f) == 1.f && wf::TallScale(32.f / 9.f) == 1.f, "wide.vertical_kept");

  // ---- the full-view test by shape ----
  {
    const q::Quad2D hud = Rect(40.f, 600.f, 360.f, 700.f);   // a health bar: not the whole view
    Check(!wf::FullViewRow(hud), "fullview.hud_not_full");
    q::Quad2D spr = Rect(0.f, 0.f, 1280.f, 720.f);
    spr.kind = q::kQkSprite;
    Check(!wf::FullViewRow(spr), "fullview.sprite_not_full");
    q::Quad2D proj = Rect(0.f, 0.f, 1280.f, 720.f);
    proj.use_proj = 1.f;
    Check(!wf::FullViewRow(proj), "fullview.matrix_placed_not_full");
    q::Quad2D nover = Rect(0.f, 0.f, 1280.f, 720.f);
    nover.have = q::kQhVerts;
    Check(!wf::FullViewRow(nover), "fullview.needs_pos_scale");
    const q::Quad2D edge = Rect(0.5f, 0.5f, 1280.5f, 720.5f);   // the game's own corner convention
    Check(wf::FullViewRow(edge) == (g_plant != 4), "fullview.fade_game_corners");
  }

  // ---- the guest read-back crop (a wide picture -> its centred 16:9) ----
  {
    const uint32_t w = 1680, h = 720;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) px[(size_t(y) * w + x) * 4] = uint8_t(x & 0xFF), px[(size_t(y) * w + x) * 4 + 1] = uint8_t(x >> 8);
    uint32_t cw = 0, ch = 0;
    const std::vector<uint8_t> c = wf::CropBox(px.data(), w, h, 4, 1280, 720, &cw, &ch);
    const uint32_t first = c.size() >= 2 ? uint32_t(c[0]) | (uint32_t(c[1]) << 8) : 0, last_off = (size_t(cw) - 1) * 4;
    const uint32_t last = c.size() > last_off + 1 ? uint32_t(c[last_off]) | (uint32_t(c[last_off + 1]) << 8) : 0;
    Check(cw == 1280 && ch == 720 && c.size() == size_t(1280) * 720 * 4 && first == 200 && last == 200 + 1279, "box.crop_pixels", "w " + std::to_string(cw) + " first " + std::to_string(first) + " last " + std::to_string(last));
    // a render-scaled (2x) picture crops by the same fractions
    const wf::Box b2 = wf::BoxOf(3360, 1440);
    Check(b2.w == 2560 && b2.h == 1440 && b2.x == 400, "box.crop_scaled", std::to_string(b2.x));
    // a 16:9 picture is returned whole
    std::vector<uint8_t> px2(size_t(1280) * 720 * 4, 7);
    const std::vector<uint8_t> c2 = wf::CropBox(px2.data(), 1280, 720, 4, 1280, 720, &cw, &ch);
    Check(cw == 1280 && ch == 720 && c2 == px2, "box.crop_identity");
  }

  // ---- the 2D drawer's viewport / scissor decision (quads2d_draw.cpp Append -> wide_frame.h RowViewOf) on a 1706x720 F holding the game's 1280x720 box at x 213 ----
  {
    const uint32_t gw = 1280, gh = 720, tw = 1706, th = 720;
    const int32_t ox = g_plant == 6 ? 0 : 213, oy = 0;
    const wf::IRect whole_box{213, 0, gw, gh}, panel{213 + 100, 50, 400, 300};
    // a full-view fade with no scissor: the whole target
    wf::RowView v = wf::RowViewOf(gw, gh, tw, th, ox, oy, true, false, wf::IRect{});
    Check(v.full_target && v.viewport == wf::IRect{0, 0, tw, th} && v.scissor == wf::IRect{0, 0, tw, th}, "rowview.fade_whole_target");
    // the same fade with the game's whole-frame scissor: still the whole target (the scissor is the whole box, not a panel)
    v = wf::RowViewOf(gw, gh, tw, th, ox, oy, true, true, whole_box);
    Check(v.full_target && v.scissor == wf::IRect{0, 0, tw, th}, "rowview.fade_whole_scissor");
    // a flat tint the game clips to a panel stays in the box and keeps its (offset) scissor
    v = wf::RowViewOf(gw, gh, tw, th, ox, oy, true, true, panel, g_plant == 5);
    Check(!v.full_target && v.viewport == whole_box && v.scissor == panel, "rowview.panel_tint_boxed");
    // a HUD row: the box viewport at the box origin, its own scissor
    v = wf::RowViewOf(gw, gh, tw, th, ox, oy, false, g_plant != 7, panel);
    Check(!v.full_target && v.viewport == whole_box, "rowview.box_origin", "x " + std::to_string(v.viewport.x));
    Check(v.scissor == panel, "rowview.hud_scissor");
    // a HUD row with no scissor: the box viewport, the whole target as scissor (the box viewport clips it)
    v = wf::RowViewOf(gw, gh, tw, th, ox, oy, false, false, wf::IRect{});
    Check(v.viewport == whole_box && v.scissor == wf::IRect{0, 0, tw, th}, "rowview.hud_no_scissor");
    // 16:9: the target is the game's frame, nothing widens, the old viewport / scissor
    v = wf::RowViewOf(gw, gh, gw, gh, 0, 0, true, false, wf::IRect{});
    Check(!v.full_target && v.viewport == wf::IRect{0, 0, gw, gh} && v.scissor == wf::IRect{0, 0, gw, gh}, "rowview.identity_16_9");
    // the in-scene 2D box of a render-scaled (2x) scene colour of the 1706x720 shape: 2560x1080 -> 3412x1440 holds the box at 426 (213 x 2)
    wf::IRect sb = wf::SceneBoxOf(3412, 1440, 1280, 720, 1706, 720);
    if (g_plant == 8) sb.x = 0;
    Check(sb.w == 2560 && sb.h == 1440 && sb.x == 426 && sb.y == 0, "scenebox.centred", "x " + std::to_string(sb.x) + " w " + std::to_string(sb.w));
    const wf::IRect id = wf::SceneBoxOf(1280, 720, 1280, 720, 1280, 720);
    Check(id.x == 0 && id.w == 1280 && id.h == 720, "scenebox.identity_16_9");
  }
  // an even size is what the post passes get (review F5)
  for (double asp : {21.0 / 9.0, 2560.0 / 1080.0, 3440.0 / 1440.0, 1.5, 1.6, 4.0 / 3.0}) {
    float k, t;
    Factors(asp, k, t);
    const wf::FrameShape s = wf::Shape(1280, 720, k, t, true);
    Check(s.w % 2 == 0 && s.h % 2 == 0, "shape.even", std::to_string(s.w) + "x" + std::to_string(s.h));
  }

  std::printf("wide_frame_unit: %d ok, %d fail%s\n", g_ok, g_fail, g_plant ? (" (plant " + std::to_string(g_plant) + ")").c_str() : "");
  return g_fail ? 1 : 0;
}
