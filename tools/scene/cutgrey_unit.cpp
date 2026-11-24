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
// flash_v_flip (the centre's depth row read upside down) -> [MAP]; flash_fetch_row0 (the centre's depth read from row 0) -> [OCC].
// [OCC] (S110 FLASHOCC, docs\research\s110\FLASHOCC.md): what a live cutscene frame has and [MAP] does not: a camera made as the live record frame makes it (record_frame.cpp:381-400: a game-space view, the FOV
// projection with near 10 / far 300000, then the view's y row and the eye's y negated into the viewer's space), the depth WRITTEN BY REAL DRAWS (an opaque yBG_Default quad, and a skinned yCh_skin quad through the
// character path) and SCALED targets (render scale 1, 1.5, 2; the post chain on). A body 520 units in front of the camera covers the centre of a flash 3600 units out: the picture with the flash equals the one
// without it; with the body moved aside the same flash is drawn. [FACE]: FlashSpriteFacing (flash_sprite.h) turns the sprite toward the frame's viewer eye. [NEAR]: the census counts a flash nearer than
// kFlashNearViewZ (600 units: near 1) and not the far ones (3600 units), and the flashes line says so.
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
#include "../../src/renderer_scene/flash_sprite.h"
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

// ---- [OCC] -----------------------------------------------------------------------------------------------------------------------

void Norm3(float v[3]) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  for (int i = 0; i < 3; ++i) v[i] /= l;
}
void Cross3(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0]; }
void ToViewer(const float g[3], float v[3]) { v[0] = g[0], v[1] = -g[1], v[2] = g[2]; }   // rf_c_core.cpp PointToViewer's rule

// A cutscene camera as the live record frame has it (record_frame.cpp:381-400): the game's view (+x right, +y DOWN, +z forward), the projection built from the FOV for the target (near 10, far 300000, the y scale
// negative), then the live conversion: the view's y input row (m[4..7]) and the eye's y negated (the viewer's space).
struct OccCam { Mat4 view, proj; float eye[3]; float q, nq; };
OccCam MakeOccCam(const float eye_g[3], const float look_g[3]) {
  float f[3] = {look_g[0] - eye_g[0], look_g[1] - eye_g[1], look_g[2] - eye_g[2]};
  Norm3(f);
  const float down[3] = {0.f, 1.f, 0.f};
  float r[3], d[3];
  Cross3(down, f, r);
  Norm3(r);
  Cross3(f, r, d);
  OccCam c;
  c.view = IdentityMat4();
  const float* ax[3] = {r, d, f};
  for (int i = 0; i < 3; ++i) {
    for (int k = 0; k < 3; ++k) c.view.m[k * 4 + i] = ax[i][k];
    c.view.m[12 + i] = -(eye_g[0] * ax[i][0] + eye_g[1] * ax[i][1] + eye_g[2] * ax[i][2]);
  }
  const float aspect = float(W) / float(H), n = 10.f, fz = 300000.f;
  const float ys = 1.f / std::tan(44.f * 3.14159265f / 360.f), xs = ys / aspect;
  c.proj = IdentityMat4();
  c.proj.m[0] = xs, c.proj.m[5] = -ys, c.proj.m[10] = fz / (fz - n), c.proj.m[11] = 1.f, c.proj.m[14] = -n * fz / (fz - n), c.proj.m[15] = 0.f;
  for (int k = 4; k < 8; ++k) c.view.m[k] = -c.view.m[k];
  ToViewer(eye_g, c.eye);
  c.q = fz / (fz - n), c.nq = n * fz / (fz - n);
  return c;
}

// The occluder: a +-1 quad in its own x-y plane, opaque. skinned = a yCh_skin mesh on bone 0 (the character path), else a yBG_Default arena mesh.
assets::Model Occluder(bool skinned) {
  assets::Model m;
  m.label = skinned ? "occ_body" : "occ_quad";
  assets::Texture t;
  t.name = "color", t.format = assets::TexFormat::kR8G8B8A8, t.width = t.height = 4, t.mips = 1;
  for (int i = 0; i < 16; ++i) t.data.push_back(200), t.data.push_back(140), t.data.push_back(110), t.data.push_back(255);
  m.textures.push_back(t);
  assets::Material mat;
  mat.shader = skinned ? "yCh_skin" : "yBG_Default";
  mat.diffuse = 0;
  for (int c = 0; c < 4; ++c) mat.ambient[c] = 1.f, mat.diffuse_color[c] = 1.f;
  m.materials.push_back(mat);
  assets::Mesh me;
  me.part = "body";
  me.material = 0;
  me.skinned = skinned;
  const float cx[4] = {-1, 1, 1, -1}, cy[4] = {-1, -1, 1, 1};
  for (int i = 0; i < 4; ++i) {
    assets::Vertex vx{};
    vx.pos[0] = cx[i], vx.pos[1] = cy[i], vx.pos[2] = 0.f;
    vx.normal[2] = -1.f;
    vx.uv0[0] = cx[i] > 0 ? 1.f : 0.f, vx.uv0[1] = cy[i] > 0 ? 1.f : 0.f;
    vx.color = 0xFFFFFFFFu;
    vx.weights[0] = 1.f;
    me.vertices.push_back(vx);
  }
  me.indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};   // both windings: drawn whatever the cull
  m.meshes.push_back(me);
  return m;
}

struct OccPic { std::vector<uint8_t> px; uint32_t w = 0, h = 0; };

// One cutscene picture: the occluder (rigid or skinned) at `occ_v` (viewer space), facing the eye, 70 units half-size; optionally the flash sprite at `flash_v`, 700 units half-size, through FlashSpriteFacing.
OccPic DrawOcc(rdr::Renderer& renderer, FrontEnd& fe, int flash_model, int occ_model, bool skinned, const OccCam& cam, const float occ_v[3], const float flash_v[3], bool with_flash, uint32_t& val) {
  SceneFrame sf;
  sf.width = W, sf.height = H;
  sf.viewer_space = true;
  sf.clear[0] = 0.05f, sf.clear[1] = 0.05f, sf.clear[2] = 0.08f, sf.clear[3] = 1.f;
  sf.camera.view = cam.view;
  sf.camera.proj = cam.proj;
  std::memcpy(sf.camera.eye, cam.eye, sizeof cam.eye);
  PostParams pp = DefaultPostParams(W, H, false);
  pp.depth_from_viewer = true, pp.proj_q = cam.q, pp.proj_nq = cam.nq;
  sf.post = &pp;
  const assets::Matrix ow = FlashWorld(occ_v, 70.f, cam.eye);
  Mat4 om;
  std::memcpy(om.m, ow.data(), sizeof om.m);
  Object ob;
  ob.asset = uint32_t(occ_model);
  if (skinned) {
    sf.bones.push_back(om);
    ob.flags = kObjVisible | kObjSkinned;
    ob.world = IdentityMat4();
    ob.bones_first = 0, ob.bones_count = 1;
  } else {
    ob.world = om;
  }
  sf.objects.push_back(ob);
  if (with_flash) {
    FlashSprite s = FlashSpriteFacing(sf, flash_v, 700.f, flash_model);
    s.rgb[0] = 0.6f, s.rgb[1] = 0.8f, s.rgb[2] = 0.6f, s.alpha = 0.5f;
    sf.flashes.push_back(s);
  }
  OccPic p;
  std::string err;
  if (!fe.RenderToPixels(sf, p.px, err, &p.w, &p.h)) std::printf("render failed: %s\n", err.c_str());
  val += fe.validation_errors();
  renderer.WaitIdle();
  return p;
}

int Differ(const OccPic& a, const OccPic& b, int tol) {   // channel values that differ by more than tol
  if (a.px.size() != b.px.size() || a.px.empty()) return -1;
  int n = 0;
  for (size_t i = 0; i + 3 < a.px.size(); i += 4)
    for (int c = 0; c < 3; ++c) n += std::abs(int(a.px[i + size_t(c)]) - int(b.px[i + size_t(c)])) > tol;
  return n;
}
int LitPixels(const OccPic& with, const OccPic& without) {   // pixels the flash lit
  if (with.px.size() != without.px.size() || with.px.empty()) return -1;
  int n = 0;
  for (size_t i = 0; i + 3 < with.px.size(); i += 4) {
    bool lit = false;
    for (int c = 0; c < 3; ++c) lit = lit || int(with.px[i + size_t(c)]) - int(without.px[i + size_t(c)]) > 4;
    n += lit;
  }
  return n;
}

// [OCC] at one render scale. Returns the validation errors.
uint32_t RunOcc(float scale) {
  uint32_t val = 0;
  std::string err;
  rdr::RendererOptions ro;
  ro.validation = 1;
  ro.render_scale = scale;
  auto renderer = rdr::Renderer::Create(ro, err);
  char tag[64];
  std::snprintf(tag, sizeof tag, "scale %.1f", double(scale));
  if (!renderer) { Check(false, "OCC", std::string(tag) + ": no headless renderer: " + err); return 0; }
  FrontEnd fe(*renderer);
  fe.SetScaledTargets(scale != 1.f);   // as the stream-free host does (live_scene_draw.cpp EnsureFrontEnd)
  std::vector<std::string> warnings;
  const int flash_model = fe.AddModel(FlashModel(), warnings);
  const int quad_model = fe.AddModel(Occluder(false), warnings);
  const int body_model = fe.AddModel(Occluder(true), warnings);
  // A cutscene close-up: the camera inside the ring, a body 520 units away, a crowd flash on the line through the body 3600 units out (the spawner's radii are 1650+, FLASHDEPTH.md a5).
  const float eye_g[3] = {300.f, -160.f, -420.f}, body_g[3] = {0.f, -120.f, 0.f}, look_g[3] = {40.f, -95.f, 0.f};
  const OccCam cam = MakeOccCam(eye_g, look_g);
  float flash_g[3], aside_g[3];
  for (int i = 0; i < 3; ++i) flash_g[i] = eye_g[i] + (body_g[i] - eye_g[i]) * (3600.f / 520.f);
  aside_g[0] = body_g[0] - 260.f, aside_g[1] = body_g[1], aside_g[2] = body_g[2] - 180.f;   // the body moved aside: the flash centre's texel is the far clear
  float body_v[3], flash_v[3], aside_v[3];
  ToViewer(body_g, body_v), ToViewer(flash_g, flash_v), ToViewer(aside_g, aside_v);
  for (int sk = 0; sk < 2; ++sk) {
    const bool skinned = sk == 1;
    const int occ = skinned ? body_model : quad_model;
    const std::string what = std::string(tag) + ", " + (skinned ? "skinned yCh_skin body" : "opaque yBG_Default quad");
    const OccPic a = DrawOcc(*renderer, fe, flash_model, occ, skinned, cam, body_v, flash_v, false, val);   // the body over the centre, no flash
    const OccPic b = DrawOcc(*renderer, fe, flash_model, occ, skinned, cam, body_v, flash_v, true, val);    // the body over the centre, the flash
    const OccPic c = DrawOcc(*renderer, fe, flash_model, occ, skinned, cam, aside_v, flash_v, false, val);  // the body aside, no flash
    const OccPic d = DrawOcc(*renderer, fe, flash_model, occ, skinned, cam, aside_v, flash_v, true, val);   // the body aside, the flash
    const int body_px = Differ(a, c, 4), hidden_diff = Differ(b, a, 1), drawn_px = LitPixels(d, c);
    const int need = int(200.f * scale * scale);
    std::printf("info: [OCC] %s: picture %ux%u, the body changed %d channel values, the covered flash changed %d, the uncovered flash lit %d pixels\n", what.c_str(), a.w, a.h, body_px, hidden_diff, drawn_px);
    Check(a.w == uint32_t(std::ceil(float(W) * scale)) && a.h == uint32_t(std::ceil(float(H) * scale)), "OCC", what + ": the picture is drawn at the render scale (" + std::to_string(a.w) + "x" + std::to_string(a.h) + ")");
    Check(body_px > need, "OCC", what + ": the body is drawn (" + std::to_string(body_px) + " channel values differ from the picture with the body aside)");
    Check(drawn_px > need, "OCC", what + ": the same flash with the body aside is drawn (" + std::to_string(drawn_px) + " pixels lit)");
    Check(hidden_diff == 0, "OCC", what + ": a body in front of the flash centre hides the whole sprite (the picture equals the one without it): " + std::to_string(hidden_diff) + " channel values differ");
  }
  if (scale == 1.f) {   // [NEAR] (S110 FLASHOCC census): the far flashes above are not near; one 600 units out is, and `nearest` is its view z
    const uint64_t near0 = fe.flash_near();
    const float nearest0 = fe.flash_nearest();
    float near_g[3], near_v[3];
    for (int i = 0; i < 3; ++i) near_g[i] = eye_g[i] + (body_g[i] - eye_g[i]) * (600.f / 520.f);
    ToViewer(near_g, near_v);
    (void)DrawOcc(*renderer, fe, flash_model, quad_model, false, cam, aside_v, near_v, true, val);
    const uint64_t near1 = fe.flash_near();
    const float nearest1 = fe.flash_nearest();
    std::printf("info: [NEAR] threshold %.0f: after the far flashes near %llu nearest %.1f; after one 600 units out near %llu nearest %.1f; line: %s\n", double(kFlashNearViewZ), (unsigned long long)near0, double(nearest0),
                (unsigned long long)near1, double(nearest1), fe.FlashLine().c_str());
    Check(near0 == 0 && nearest0 > kFlashNearViewZ, "NEAR", "the flashes 3600 units out are not near (near " + std::to_string(near0) + ", nearest " + std::to_string(nearest0) + ")");
    Check(near1 == 1 && nearest1 > 450.f && nearest1 < 600.f && fe.FlashLine().find("near(view z < 1500) 1 nearest") != std::string::npos, "NEAR",
          "a flash 600 units out counts near 1 and is the nearest (" + std::to_string(nearest1) + ")");
  }
  return val;
}

// [FACE] (FLASHDEPTH.md b8): the sprite faces the frame's eye, which is the VIEWER's (y negated), not the record's game-space eye.
void CheckFace() {
  const float eye_g[3] = {300.f, -160.f, -420.f}, look_g[3] = {40.f, -95.f, 0.f}, flash_g[3] = {-1800.f, 160.f, 2900.f};
  const OccCam cam = MakeOccCam(eye_g, look_g);
  SceneFrame sf;
  std::memcpy(sf.camera.eye, cam.eye, sizeof cam.eye);
  float flash_v[3];
  ToViewer(flash_g, flash_v);
  const FlashSprite s = FlashSpriteFacing(sf, flash_v, 2.f, 0);
  float toward[3] = {s.world[8], s.world[9], s.world[10]}, want[3], game[3];
  for (int i = 0; i < 3; ++i) want[i] = cam.eye[i] - flash_v[i], game[i] = eye_g[i] - flash_v[i];
  Norm3(toward), Norm3(want), Norm3(game);
  const float dv = toward[0] * want[0] + toward[1] * want[1] + toward[2] * want[2], dg = toward[0] * game[0] + toward[1] * game[1] + toward[2] * game[2];
  Check(dv > 0.99999f && dg < 0.9999f && std::fabs(s.centre[1] - flash_v[1]) < 1e-3f, "FACE", "the sprite's toward-eye row points at the frame's viewer eye: cos " + std::to_string(dv) + " (against the game-space eye: cos " + std::to_string(dg) + ")");
}

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
      else if (p == "flash_fetch_row0") g_plant_flash_fetch_row0.store(true);
      else return std::fprintf(stderr, "unknown plant %s\n", p.c_str()), 2;
    } else if (a == "--expect-fail") expect_fail = true;
    else return std::fprintf(stderr, "usage: cutgrey_unit [--plant flash_pre_post | flash_file_colour | flash_v_flip | flash_fetch_row0] [--expect-fail]\n"), 2;
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
  CheckFace();
  for (const float scale : {1.f, 1.5f, 2.f}) val += RunOcc(scale);
  Check(val == 0, "VAL", "0 validation errors: " + std::to_string(val));
  if (expect_fail) {
    std::printf(g_bad ? "planted twin CAUGHT (%d check(s) failed: %s): PASS\n" : "planted twin NOT caught: FAIL\n", g_bad, g_failed.c_str());
    return g_bad ? 0 : 1;
  }
  std::printf(g_bad ? "FAIL: %d check(s): %s\n" : "PASS: cutgrey_unit\n", g_bad, g_failed.c_str());
  return g_bad ? 1 : 0;
}
