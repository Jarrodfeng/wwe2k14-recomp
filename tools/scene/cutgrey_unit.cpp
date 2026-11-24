// body-scale: none (front-end unit, no wrestler is drawn)  [S103 RGF: body_scale_feed_lint.py]
// S110 CUTGREY unit (docs\research\s110\CUTGREY.md, CUTGREY_BUILD.md): the crowd camera flash is drawn AFTER the post chain, with the colour of its row, lit by the game's rule, hidden whole by the depth at its
// centre. Through the REAL FrontEnd (AddModel + RenderToPixels, a headless Vulkan renderer; 0 game runs, no game files: the sprite model is a hand-made quad with a white texture).
//   cutgrey_unit [--plant flash_pre_post | flash_file_colour] [--expect-fail]
// The planted frame: a dim uniform scene whose depth is the far plane (so the depth of field's far blur applies everywhere), the match's depth of field + haze, the sepia (yMonotone) on, one flash sprite
// of the row colour (0.6, 0.8, 0.6) at alpha 0.5 beyond the focus. Four pictures of the same frame:
//   R0  the scene with the post chain, no flash                       R1  the same with the flash sprite
//   RS  the sprite alone: no post, black scene (what the sprite adds)  R3  the same flash with its CENTRE planted beyond the far plane (hidden whole)
// Checks: [ORDER] R1 - R0 equals RS on every pixel (post(scene) + the additive sprite: nothing of our blur / haze / sepia touched the sprite);  [LIGHT] the sprite's centre pixel is colour x (1 + abs(n.x)) x alpha
// (analytic: |n.x| = 0.2 -> 1.2 x (0.6, 0.8, 0.6) x 0.5 = (0.36, 0.48, 0.36) = (92, 122, 92); the fragment colour is clamped to 1 before the blend, as on any UNORM target, so the unit keeps the lit colour below 1);
// [COLOUR] the green / red ratio of the sprite is the row's 0.8 / 0.6;  [HIDE] with the centre beyond the far plane the picture is R0 exactly while the sprite's own
// pixels are in front of the depth (hidden WHOLE, not per pixel);  [VAL] 0 validation errors.
// [MAP] (S110 CUTGREY review): the scene depth is NOT uniform: a stage hook clears the TOP half of the depth to 0.3 (nearer than any flash centre here), no post, black scene. A flash whose centre is in the top half
// (world y +1) is hidden whole (the picture equals the one without it); the same flash in the bottom half (y -1) is drawn. So the centre's depth is read at the centre's own texel, not anywhere else.
// Planted twins (each must FAIL its named check): flash_pre_post (the old order: the flash drawn into the scene colour before the post chain) -> [ORDER]; flash_file_colour (the row's colour ignored) -> [COLOUR];
// flash_v_flip (the centre's depth row read upside down) -> [MAP].
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/post_pass.h"
#include "../../src/renderer_scene/record_io.h"

using namespace rdr_scene;

namespace {

constexpr uint32_t W = 128, H = 72;
std::string g_failed;
int g_bad = 0;

void Check(bool ok, const char* tag, const std::string& what) {
  std::printf("%s  [%s] %s\n", ok ? "PASS" : "FAIL", tag, what.c_str());
  if (!ok) {
    ++g_bad;
    if (g_failed.find(tag) == std::string::npos) g_failed += std::string(g_failed.empty() ? "" : ", ") + "[" + tag + "]";
  }
}

// The flash model: one quad, +-1 across in x / y, unit normal (0.2, 0, 0.9798) (so the world's turned normal has |n.x| = 0.2), a white 4x4 texture, plain white vertex colour.
assets::Model FlashModel() {
  assets::Model m;
  m.label = "cutgrey_flash";
  assets::Texture t;
  t.name = "flash01", t.format = assets::TexFormat::kR8G8B8A8, t.width = t.height = 4, t.mips = 1;
  t.data.assign(4 * 4 * 4, 255);
  m.textures.push_back(t);
  assets::Material mat;
  mat.shader = "yFlashDefault";
  mat.diffuse = 0;
  mat.alpha_blend = true, mat.additive = true;
  for (int c = 0; c < 4; ++c) mat.ambient[c] = 1.f, mat.diffuse_color[c] = 1.f;
  m.materials.push_back(mat);
  assets::Mesh me;
  me.part = "flash 31000";
  me.material = 0;
  const float cx[4] = {-1, 1, 1, -1}, cy[4] = {-1, -1, 1, 1};
  for (int i = 0; i < 4; ++i) {
    assets::Vertex vx{};
    vx.pos[0] = cx[i], vx.pos[1] = cy[i], vx.pos[2] = 0.f;
    vx.normal[0] = 0.2f, vx.normal[2] = 0.9797959f;
    vx.uv0[0] = cx[i] > 0 ? 1.f : 0.f, vx.uv0[1] = cy[i] > 0 ? 1.f : 0.f;
    vx.color = 0xFFFFFFFFu;
    vx.weights[0] = 1.f;
    me.vertices.push_back(vx);
  }
  me.indices = {0, 1, 2, 0, 2, 3};
  m.meshes.push_back(me);
  return m;
}

// view-projection of a camera at the origin looking down +z (row vectors, clip = [p 1] * M): near 1, far 100, x / y scaled by 2 (a ~53 degree view); depth 0..1 forward
struct Cam { Mat4 vp; float q, nq; };
Cam MakeCam() {
  Cam c;
  c.vp = IdentityMat4();
  const float zn = 1.f, zf = 100.f;
  c.q = zf / (zf - zn);
  c.nq = c.q * zn;
  c.vp.m[0] = 2.f * float(H) / float(W), c.vp.m[5] = 2.f;   // square pixels: x scaled by 1 / aspect
  c.vp.m[10] = c.q, c.vp.m[11] = 1.f, c.vp.m[14] = -c.nq, c.vp.m[15] = 0.f;
  return c;
}

// [MAP]'s occluder: the top half of the scene depth cleared to 0.3 right after the frame's clear (no geometry, no shader: only the depth differs between the halves).
struct TopHalfNear : StageHook {
  const char* Name() const override { return "cutgrey_top_half_near"; }
  void Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& fb) override {
    if (stage != Stage::BeforeOpaque) return;
    rdr::ClearOp c;
    c.flags = rdr::kClearDepth, c.full = 0, c.depth = 0.3f;
    c.rect = {0, 0, ctx.width, ctx.height / 2};
    fb.Clear(c);
  }
};

struct Pic { std::vector<uint8_t> px; };

Pic Draw(rdr::Renderer& renderer, FrontEnd& fe, int model, bool with_post, bool with_flash, float centre_z, const float rgb[3], uint32_t& val_errors, float y = 0.f, StageHook* hook = nullptr) {
  const Cam cam = MakeCam();
  SceneFrame sf;
  sf.width = W, sf.height = H;
  sf.viewer_space = false;
  const float dim = with_post ? 0.06f : 0.f;   // the scene is dim and uniform (the sprite adds on top, no clamping); the reference RS is a black scene
  sf.clear[0] = dim, sf.clear[1] = dim, sf.clear[2] = dim, sf.clear[3] = 1.f;
  sf.camera.view = cam.vp;
  sf.camera.proj = IdentityMat4();
  const float eye[3] = {0.f, 0.f, 0.f};
  std::memcpy(sf.camera.eye, eye, sizeof eye);
  PostParams pp = DefaultPostParams(W, H, false);   // the match's depth of field + haze
  pp.film = true;                                    // the sepia
  for (int i = 0; i < 4; ++i) pp.film_cb[i] = -0.06f, pp.film_cr[i] = 0.09f, pp.film_alpha[i] = 0.85f;
  pp.depth_from_viewer = true, pp.proj_q = cam.q, pp.proj_nq = cam.nq;
  if (with_post) sf.post = &pp;
  if (hook) sf.stage_hooks.push_back(hook);
  if (with_flash) {
    FlashSprite s;
    s.model = model;
    const float scale = 2.f;   // the quad is +-1: the sprite is 2 units across at distance 5
    s.world[0] = -scale, s.world[5] = scale, s.world[10] = -scale, s.world[13] = y, s.world[14] = 5.f;   // the camera-facing frame of a sprite at (0, y, 5) seen from the origin (right = -x, up = y, toward the eye = -z)
    s.centre[0] = 0.f, s.centre[1] = y, s.centre[2] = centre_z;
    std::memcpy(s.rgb, rgb, sizeof s.rgb);
    s.alpha = 0.5f;
    sf.flashes.push_back(s);
  }
  Pic p;
  std::string err;
  if (!fe.RenderToPixels(sf, p.px, err)) std::printf("render failed: %s\n", err.c_str());
  val_errors += fe.validation_errors();
  renderer.WaitIdle();
  return p;
}

int At(const Pic& p, uint32_t x, uint32_t y, int c) { return p.px.size() >= size_t(W) * H * 4 ? int(p.px[(size_t(y) * W + x) * 4 + size_t(c)]) : 0; }

}  // namespace

int main(int argc, char** argv) {
  bool expect_fail = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) {
      const std::string p = argv[++i];
      if (p == "flash_pre_post") g_plant_flash_pre_post.store(true);
      else if (p == "flash_file_colour") g_plant_flash_file_colour.store(true);
      else if (p == "flash_v_flip") g_plant_flash_v_flip.store(true);
      else return std::fprintf(stderr, "unknown plant %s\n", p.c_str()), 2;
    } else if (a == "--expect-fail") expect_fail = true;
    else return std::fprintf(stderr, "usage: cutgrey_unit [--plant flash_pre_post | flash_file_colour | flash_v_flip] [--expect-fail]\n"), 2;
  }
  std::string err;
  rdr::RendererOptions ro;
  ro.validation = 1;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) return std::fprintf(stderr, "no headless renderer: %s\n", err.c_str()), 2;
  FrontEnd fe(*renderer);
  std::vector<std::string> warnings;
  const int model = fe.AddModel(FlashModel(), warnings);
  uint32_t val = 0;
  const float rgb[3] = {0.6f, 0.8f, 0.6f};
  const Pic r0 = Draw(*renderer, fe, model, true, false, 5.f, rgb, val);     // post(scene)
  const Pic r1 = Draw(*renderer, fe, model, true, true, 5.f, rgb, val);      // post(scene) + the flash
  const Pic rs = Draw(*renderer, fe, model, false, true, 5.f, rgb, val);     // the sprite alone
  const Pic r3 = Draw(*renderer, fe, model, true, true, 500.f, rgb, val);    // the centre beyond the far plane: hidden whole
  TopHalfNear near_top;
  const Pic o0 = Draw(*renderer, fe, model, false, false, 5.f, rgb, val, 0.f, &near_top);    // [MAP] the top half nearer, no flash
  const Pic ot = Draw(*renderer, fe, model, false, true, 5.f, rgb, val, 1.f, &near_top);     // [MAP] the flash's centre in the top half: hidden whole
  const Pic ob = Draw(*renderer, fe, model, false, true, 5.f, rgb, val, -1.f, &near_top);    // [MAP] the flash's centre in the bottom half: drawn
  const bool have = r0.px.size() == size_t(W) * H * 4 && r1.px.size() == r0.px.size() && rs.px.size() == r0.px.size() && r3.px.size() == r0.px.size();
  Check(have, "VAL", "four pictures of " + std::to_string(W) + "x" + std::to_string(H) + " drawn");
  if (have) {
    // the sprite's footprint: pixels RS lights
    int footprint = 0, worst = 0, hide_diff = 0, r3_vs_r0 = 0;
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        bool lit = false;
        for (int c = 0; c < 3; ++c) lit = lit || At(rs, x, y, c) > 4;
        footprint += lit;
        for (int c = 0; c < 3; ++c) {
          const int d1 = At(r1, x, y, c) - At(r0, x, y, c);
          worst = std::max(worst, std::abs(d1 - At(rs, x, y, c)));
          r3_vs_r0 += std::abs(At(r3, x, y, c) - At(r0, x, y, c)) > 1;
        }
      }
    const uint32_t cx = W / 2, cy = H / 2;
    const int sr = At(rs, cx, cy, 0), sg = At(rs, cx, cy, 1), sb = At(rs, cx, cy, 2);
    const int dr = At(r1, cx, cy, 0) - At(r0, cx, cy, 0), dg = At(r1, cx, cy, 1) - At(r0, cx, cy, 1), db = At(r1, cx, cy, 2) - At(r0, cx, cy, 2);
    std::printf("info: sprite footprint %d px; centre RS (%d %d %d), R1-R0 (%d %d %d); R0 centre (%d %d %d)\n", footprint, sr, sg, sb, dr, dg, db, At(r0, cx, cy, 0), At(r0, cx, cy, 1), At(r0, cx, cy, 2));
    Check(footprint > 400, "ORDER", "the sprite is drawn (" + std::to_string(footprint) + " pixels)");
    Check(worst <= 4, "ORDER", "R1 - R0 equals the sprite alone on every pixel (post(scene) + the additive sprite; the far blur, haze and sepia never touch it): worst difference " + std::to_string(worst) + " levels");
    Check(std::abs(sr - 92) <= 3 && std::abs(sg - 122) <= 3 && std::abs(sb - 92) <= 3, "LIGHT", "the sprite's centre is colour x (1 + abs(n.x)) x alpha = 1.2 x (0.6, 0.8, 0.6) x 0.5 = (92, 122, 92): got (" + std::to_string(sr) + " " + std::to_string(sg) + " " + std::to_string(sb) + ")");
    const float ratio = dr > 0 ? float(dg) / float(dr) : 0.f;
    Check(std::fabs(ratio - 0.8f / 0.6f) < 0.12f && std::abs(dr - db) <= 3, "COLOUR", "the sprite on the finished picture takes the row's colour (0.6, 0.8, 0.6): green / red " + std::to_string(ratio) + " (want 1.333), red - blue " + std::to_string(dr - db));
    Check(r3_vs_r0 == 0 && footprint > 400, "HIDE", "the centre beyond the far plane hides the whole sprite (the picture is R0): " + std::to_string(r3_vs_r0) + " channel values differ while the sprite covers " + std::to_string(footprint) + " pixels");
    (void)hide_diff;
  }
  const bool have_map = o0.px.size() == size_t(W) * H * 4 && ot.px.size() == o0.px.size() && ob.px.size() == o0.px.size();
  Check(have_map, "MAP", "three occluder pictures drawn");
  if (have_map) {
    int top_diff = 0, bottom_px = 0;
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        bool b = false;
        for (int c = 0; c < 3; ++c) top_diff += std::abs(At(ot, x, y, c) - At(o0, x, y, c)) > 1, b = b || std::abs(At(ob, x, y, c) - At(o0, x, y, c)) > 4;
        bottom_px += b;
      }
    std::printf("info: [MAP] top-half flash changed %d channel values, bottom-half flash lit %d pixels\n", top_diff, bottom_px);
    Check(top_diff == 0, "MAP", "a flash whose centre is in the nearer top half is hidden whole: " + std::to_string(top_diff) + " channel values differ from the picture without it");
    Check(bottom_px > 400, "MAP", "the same flash with its centre in the far bottom half is drawn: " + std::to_string(bottom_px) + " pixels");
  }
  Check(val == 0, "VAL", "0 validation errors: " + std::to_string(val));
  if (expect_fail) {
    std::printf(g_bad ? "planted twin CAUGHT (%d check(s) failed: %s): PASS\n" : "planted twin NOT caught: FAIL\n", g_bad, g_failed.c_str());
    return g_bad ? 0 : 1;
  }
  std::printf(g_bad ? "FAIL: %d check(s): %s\n" : "PASS: cutgrey_unit\n", g_bad, g_failed.c_str());
  return g_bad ? 1 : 0;
}
