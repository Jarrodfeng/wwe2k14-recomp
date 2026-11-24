// body-scale: none (front-end unit, no wrestler is drawn)  [S103 RGF: body_scale_feed_lint.py]
// S103 RFL unit: reflections in the stream-free front end, drawn through the REAL FrontEnd (AddModel + RenderToPixels, a headless Vulkan renderer; 0 game runs).
//   rfl_unit --game DIR [--plant unbound] [--expect-fail] [--arena N] [--sheet OUT.png]
// A. FACES (synthetic, needs only the effect table from the player's pac\_startup.pac): six reflective quads, one per cube face, each viewed head-on along its own axis; the cube's six faces are six solid
//    colours (+X red, -X cyan, +Y green, -Y magenta, +Z blue, -Z yellow, DDS order). The reflection of a head-on view samples the face on the eye's side, so the centre pixel's colour names the layer the
//    front end uploaded each face to: layer i = file face i (RFF.md section A [C]) and cubes[8] = the cube the material names.
// B. REAL (needs the game files): the first arena whose yBG_Reflect meshes name a cube (cube_00 / cube_01 / cube_ref ...) with g_fReflectAlpha > 0. Those meshes are drawn with the switch ON, with the cube UNBOUND
//    (same variants, plant_cube_unbound) and OFF (the S101 stand-in); the control (non-reflective yBG_Default meshes of the same arena) is drawn ON and OFF. The reflective picture must DIFFER from the unbound one
//    (the reflection itself), the control must be IDENTICAL (nothing non-reflective changes), and the census line must report the cube loaded, reflect draws > 0, no fallback.
// --plant unbound = FrontEndOptions::plant_cube_unbound (the cube is never bound: the S101 black stand-in): checks A and B1 must FAIL; --expect-fail inverts the exit code for the check row.
// Exit 0 = every check passed (with --expect-fail: only when the fault was caught), 1 = a check failed, 2 = usage / cannot read.
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
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/read_rule.h"
#include "../../src/renderer_scene/record_io.h"

using namespace rdr_scene;
using namespace assets;   // (pac::, as technique_census.cpp reads it)

namespace {

constexpr uint32_t W = 64, H = 64;
int g_bad = 0;
bool g_plant = false;
bool g_noflip = false;   // --plant noflip: FrontEndOptions::plant_no_mirror (the viewer-space draw does not flip the cube lookup)

void Check(bool ok, const std::string& what, const std::string& detail = std::string()) {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  ", detail.c_str());
  if (!ok) ++g_bad;
}

bool BuildTable(launcher::GameFiles& files) {   // = technique_census.cpp BuildTable = live_assets.cpp read_rule::BuildEffectTable (the effect part)
  pac::Pac p;
  std::string err;
  if (!pac::Open(files, "pac\\_startup.pac", p, err)) return std::fprintf(stderr, "no pac\\_startup.pac: %s\n", err.c_str()), false;
  auto table = std::make_shared<read_rule::EffectTable>();
  const auto hash = [](const uint8_t* d, size_t n) { return uint64_t(XXH3_64bits(d, n)); };
  for (const pac::Pach& pach : p.pachs)
    for (const pac::Ent& e : pach.ents) {
      const assets::Bytes dec = pac::Decode(p.bytes.data() + e.off, e.size);
      static const uint8_t kMagic[3] = {0x10, 0x2A, 0x11};
      if (dec.size() < 16 || std::search(dec.begin(), dec.end(), kMagic, kMagic + 3) == dec.end()) continue;
      read_rule::EffectTable part;
      if (!read_rule::ParseEffectLeaf(dec.data(), dec.size(), hash, part)) continue;
      for (auto& kv : part) table->emplace(kv.first, std::move(kv.second));
    }
  if (table->empty()) return std::fprintf(stderr, "no effect table parsed\n"), false;
  read_rule::PublishEffectTable(std::move(table));
  return true;
}

uint64_t Fnv(const uint8_t* b, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

// ---- A. faces ------------------------------------------------------------------------------------------------------------------

struct Rgb { int r, g, b; };
const Rgb kFaceColour[6] = {{255, 0, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {0, 0, 255}, {255, 255, 0}};   // +X -X +Y -Y +Z -Z
const uint16_t kFace565[6] = {0xF800, 0x07FF, 0x07E0, 0xF81F, 0x001F, 0xFFE0};
const char* const kFaceName[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

void Block(std::vector<uint8_t>& d, uint16_t c0, uint16_t c1) {   // one DXT1 block of the solid colour c0 (c0 > c1: four-colour mode, every index 0)
  d.push_back(uint8_t(c0 & 255)), d.push_back(uint8_t(c0 >> 8)), d.push_back(uint8_t(c1 & 255)), d.push_back(uint8_t(c1 >> 8));
  for (int i = 0; i < 4; ++i) d.push_back(0);
}

// A quad facing the +W axis `w` (a unit axis with sign), centred on the origin, 1.6 across; the stored normal points INTO the surface (the file convention, SCENE_FRONTEND.md "Space").
assets::Model Quad(const float w[3], const float u[3], const float v[3]) {
  assets::Model m;
  m.label = "rfl_quad";
  assets::Texture cube;
  cube.name = "cube_syn", cube.format = assets::TexFormat::kBC1, cube.width = cube.height = 4, cube.mips = 1, cube.faces = 6;
  for (int f = 0; f < 6; ++f) Block(cube.data, kFace565[f], 0);
  m.textures.push_back(cube);
  assets::Texture amount;   // a white 4x4 amount map: every Reflect variant (the mask / map ones read slot 9) is then bindable, scaled by 1
  amount.name = "amount", amount.format = assets::TexFormat::kBC1, amount.width = amount.height = 4, amount.mips = 1;
  Block(amount.data, 0xFFFF, 0);
  m.textures.push_back(amount);
  assets::Material mat;
  mat.shader = "yBG_Reflect";
  mat.technique = 0;
  mat.cube = 0, mat.refmap = 1, mat.reflect_alpha = 1.f;
  for (int c = 0; c < 4; ++c) mat.ambient[c] = c == 3 ? 1.f : 0.f, mat.diffuse_color[c] = c == 3 ? 1.f : 0.f;   // a black surface: the picture is the reflection alone
  mat.spec_lev = 0.f;
  m.materials.push_back(mat);
  assets::Mesh me;
  me.part = "quad";
  me.material = 0;
  const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
  for (int i = 0; i < 4; ++i) {
    assets::Vertex vx{};
    for (int k = 0; k < 3; ++k) vx.pos[k] = 0.8f * (sx[i] * u[k] + sy[i] * v[k]), vx.normal[k] = -w[k];
    vx.uv0[0] = sx[i] * 0.5f + 0.5f, vx.uv0[1] = sy[i] * 0.5f + 0.5f;
    vx.color = 0xFFFFFFFFu;
    vx.weights[0] = 1.f;
    me.vertices.push_back(vx);
  }
  me.indices = {0, 1, 2, 0, 2, 3};
  m.meshes.push_back(me);
  return m;
}

// An orthographic world -> clip (row vectors: clip = [p 1] * M) looking down -W from +W, x along U, y along V, z in 0..1 (the front-end's draw takes the matrix as is: SceneFrame::camera.view with an identity proj).
void Ortho(const float c[3], const float u[3], const float v[3], const float w[3], float hu, float hv, float rz, Mat4& out) {
  out = IdentityMat4();
  for (int i = 0; i < 3; ++i) out.m[i * 4 + 0] = u[i] / hu, out.m[i * 4 + 1] = v[i] / hv, out.m[i * 4 + 2] = w[i] / (2.f * rz), out.m[i * 4 + 3] = 0.f;
  const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  out.m[12] = -dot(c, u) / hu, out.m[13] = -dot(c, v) / hv, out.m[14] = 0.5f - dot(c, w) / (2.f * rz), out.m[15] = 1.f;
}

bool Render(FrontEnd& fe, int model, const Mat4& mvp, const float eye[3], std::vector<uint8_t>& rgba, std::string& err, bool viewer = false) {
  SceneFrame sf;
  sf.width = W, sf.height = H;
  sf.viewer_space = viewer;   // the matrices are in the vertices' own space (viewer: the viewer's Y-mirrored space, what the live record frame draws in)
  sf.clear[0] = sf.clear[1] = sf.clear[2] = 0.f, sf.clear[3] = 1.f;
  sf.camera.view = mvp;
  sf.camera.proj = IdentityMat4();
  std::memcpy(sf.camera.eye, eye, sizeof(float) * 3);
  Object ob;
  ob.asset = uint32_t(model);
  ob.world = IdentityMat4();
  sf.objects.push_back(ob);
  return fe.RenderToPixels(sf, rgba, err);
}

void TestFaces(bool viewer) {   // viewer: the quads, axes and eye are the game-space ones with y negated (the viewer's space); the cube face expected is still the GAME-space one
  const std::string sp = viewer ? "viewer space" : "game space";
  std::string err;
  rdr::RendererOptions ro;
  ro.validation = 1;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { Check(false, "faces: a headless renderer", err); return; }
  FrontEndOptions fo;
  fo.plant_cube_unbound = g_plant;
  fo.plant_no_mirror = g_noflip;
  FrontEnd fe(*renderer, fo);
  int right = 0;
  for (int f = 0; f < 6; ++f) {
    const int axis = f / 2;
    const float sgn = (f % 2) ? -1.f : 1.f;
    float w[3] = {0, 0, 0}, u[3] = {0, 0, 0}, v[3] = {0, 0, 0};
    w[axis] = sgn;
    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    u[a1] = 1.f, v[a2] = 1.f;
    if (viewer) w[1] = -w[1], u[1] = -u[1], v[1] = -v[1];   // S * (the game-space frame)
    std::vector<std::string> warnings;
    const int id = fe.AddModel(Quad(w, u, v), warnings);
    const float c0[3] = {0, 0, 0};
    const float eye[3] = {3 * w[0], 3 * w[1], 3 * w[2]};
    Mat4 mvp;
    Ortho(c0, u, v, w, 1.f, 1.f, 4.f, mvp);
    std::vector<uint8_t> px;
    if (!Render(fe, id, mvp, eye, px, err, viewer)) { Check(false, "faces (" + sp + "): render " + kFaceName[f], err); continue; }
    const uint8_t* c = &px[(size_t(H / 2) * W + W / 2) * 4];
    // the face whose colour the pixel resembles most (cosine of the colour vectors), and how strongly
    int best = -1;
    double best_cos = 0, mag = std::sqrt(double(c[0]) * c[0] + double(c[1]) * c[1] + double(c[2]) * c[2]);
    for (int k = 0; k < 6; ++k) {
      const double kn = std::sqrt(double(kFaceColour[k].r) * kFaceColour[k].r + double(kFaceColour[k].g) * kFaceColour[k].g + double(kFaceColour[k].b) * kFaceColour[k].b);
      const double cs = mag > 0 ? (double(c[0]) * kFaceColour[k].r + double(c[1]) * kFaceColour[k].g + double(c[2]) * kFaceColour[k].b) / (mag * kn) : 0;
      if (cs > best_cos) best_cos = cs, best = k;
    }
    const bool ok = best == f && best_cos > 0.97 && mag > 20.0;
    right += ok;
    char d[160];
    std::snprintf(d, sizeof d, "centre pixel (%d,%d,%d) = face %s (cos %.3f, expected %s)", c[0], c[1], c[2], best >= 0 ? kFaceName[best] : "-", best_cos, kFaceName[f]);
    Check(ok, "faces (" + sp + "): eye on " + kFaceName[f] + " samples the cube's " + kFaceName[f] + " face", d);
  }
  Check(right == 6, "faces (" + sp + "): all six cube faces reach the layer the reflection samples (layer i = file face i)", std::to_string(right) + " of 6");
  Check(fe.validation_errors() == 0, "faces (" + sp + "): 0 validation errors", std::to_string(fe.validation_errors()));
  std::printf("[rfl_unit] %s\n", fe.ReflectLine().c_str());
  Check(fe.reflect_draws() >= 6 && fe.reflect_fallbacks() == 0, "faces (" + sp + "): the census line counts the reflect draws and no fallback", fe.ReflectLine());
  renderer->WaitIdle();
}

// ---- B. real arena ----------------------------------------------------------------------------------------------------------------

struct Bounds { float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; };

// The sub-model of `full` that keeps the meshes `keep` accepts (materials and textures stay whole).
template <class Pred>
assets::Model Sub(const assets::Model& full, Pred keep, size_t cap) {
  assets::Model s = full;
  s.meshes.clear();
  for (const assets::Mesh& me : full.meshes) {
    if (s.meshes.size() >= cap) break;
    if (me.material < 0 || size_t(me.material) >= full.materials.size() || !keep(full.materials[size_t(me.material)])) continue;
    s.meshes.push_back(me);
  }
  return s;
}

Bounds BoundsOf(const assets::Model& m) {
  Bounds b;
  for (const assets::Mesh& me : m.meshes)
    for (const assets::Vertex& v : me.vertices)
      for (int k = 0; k < 3; ++k) b.lo[k] = std::min(b.lo[k], v.pos[k]), b.hi[k] = std::max(b.hi[k], v.pos[k]);
  return b;
}

struct Shot { std::vector<uint8_t> px; std::string line; uint64_t draws = 0; uint32_t fallbacks = 0, cubes = 0; bool ok = false; };

// One picture of `model` from the camera along `w` (+sign = from the +axis side); a fresh FrontEnd every time (g_reflections / the plant are read at bind time).
Shot Shoot(rdr::Renderer& renderer, const assets::Model& model, const Bounds& b, int axis, float sgn, bool reflections, bool plant) {
  Shot s;
  g_reflections.store(reflections);
  FrontEndOptions fo;
  fo.plant_cube_unbound = plant;
  FrontEnd fe(renderer, fo);
  std::vector<std::string> warnings;
  const int id = fe.AddModel(model, warnings);
  float c[3], half[3];
  for (int k = 0; k < 3; ++k) c[k] = 0.5f * (b.lo[k] + b.hi[k]), half[k] = std::max(0.05f, 0.5f * (b.hi[k] - b.lo[k]));
  float w[3] = {0, 0, 0}, u[3] = {0, 0, 0}, v[3] = {0, 0, 0};
  w[axis] = sgn;
  const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
  u[a1] = 1.f, v[a2] = 1.f;
  const float rz = 1.2f * std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]) + 1.f;
  Mat4 mvp;
  Ortho(c, u, v, w, half[a1] * 1.05f, half[a2] * 1.05f, rz, mvp);
  const float eye[3] = {c[0] + w[0] * 3.f * rz, c[1] + w[1] * 3.f * rz, c[2] + w[2] * 3.f * rz};
  std::string err;
  s.ok = Render(fe, id, mvp, eye, s.px, err);
  s.line = fe.ReflectLine();
  s.draws = fe.reflect_draws(), s.fallbacks = fe.reflect_fallbacks(), s.cubes = fe.reflect_cubes();
  renderer.WaitIdle();
  return s;
}

size_t CountDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int thresh = 6) {
  size_t n = 0;
  for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
    for (int c = 0; c < 3; ++c)
      if (std::abs(int(a[i + size_t(c)]) - int(b[i + size_t(c)])) > thresh) { ++n; break; }
  return n;
}

size_t CountCovered(const std::vector<uint8_t>& a) {   // pixels that are not the black clear colour
  size_t n = 0;
  for (size_t i = 0; i + 3 < a.size(); i += 4) n += (a[i] | a[i + 1] | a[i + 2]) != 0;
  return n;
}

void TestReal(launcher::GameFiles& files, int want_arena, const std::string& sheet) {
  std::string err;
  assets::Model full;
  int arena = 0;
  for (int id = want_arena > 0 ? want_arena : 1; id <= (want_arena > 0 ? want_arena : 80); ++id) {
    assets::Model m;
    if (!assets::LoadArena(files, id, m, err)) continue;
    size_t n = 0;
    for (const assets::Mesh& me : m.meshes) {
      if (me.material < 0) continue;
      const assets::Material& mt = m.materials[size_t(me.material)];
      n += mt.shader.rfind("yBG_Reflect", 0) == 0 && mt.cube >= 0 && mt.reflect_alpha > 0.15f;
    }
    if (n >= 4) { full = std::move(m), arena = id; break; }
  }
  Check(arena > 0, "real: an arena with yBG_Reflect meshes that name a cube (alpha > 0.15) was found", arena ? "arena " + std::to_string(arena) : err);
  if (!arena) return;
  const auto reflective = [](const assets::Material& mt) { return mt.shader.rfind("yBG_Reflect", 0) == 0 && mt.cube >= 0 && mt.reflect_alpha > 0.15f; };
  const auto plain = [](const assets::Material& mt) { return mt.shader == "yBG_Default" && mt.cube < 0; };
  const assets::Model refl = Sub(full, reflective, 400), ctrl = Sub(full, plain, 400);
  size_t cubes_in_model = 0;
  for (const assets::Texture& t : refl.textures) cubes_in_model += t.faces == 6;
  std::printf("[rfl_unit] arena %d: %zu reflective meshes, %zu control meshes, %zu cube textures in the model\n", arena, refl.meshes.size(), ctrl.meshes.size(), cubes_in_model);
  for (const assets::Texture& t : refl.textures)
    if (t.faces == 6) {
      std::printf("[rfl_unit]   cube '%s' %ux%u %u mip(s), faces:", t.name.c_str(), t.width, t.height, t.mips);
      size_t per = t.data.size() / 6;
      for (int f = 0; f < 6; ++f) std::printf(" %s=%016llx", kFaceName[f], (unsigned long long)Fnv(t.data.data() + size_t(f) * per, per));
      std::printf("\n");
    }
  Check(cubes_in_model >= 1, "real: the file cube(s) parse as six-face textures (dds.h)");
  rdr::RendererOptions ro;
  ro.validation = 1;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { Check(false, "real: a headless renderer", err); return; }
  const Bounds rb = BoundsOf(refl), cb = BoundsOf(ctrl);
  // the camera side with the larger coverage in the ON picture (the front side of the reflective surface: the file's stored normals decide the cull)
  Shot on, off;
  size_t best_cov = 0;
  int best_axis = 1;
  float best_sgn = 1.f;
  for (int axis = 0; axis < 3; ++axis)
    for (float sgn : {1.f, -1.f}) {
      Shot t = Shoot(*renderer, refl, rb, axis, sgn, true, false);
      const size_t cov = t.ok ? CountCovered(t.px) : 0;
      if (cov > best_cov) best_cov = cov, best_axis = axis, best_sgn = sgn, on = std::move(t);
    }
  (void)on;
  // THE ARM UNDER TEST: the switch on (the plant, when asked, leaves the cube unbound)
  on = Shoot(*renderer, refl, rb, best_axis, best_sgn, true, g_plant);
  off = Shoot(*renderer, refl, rb, best_axis, best_sgn, false, false);                 // the S101 stand-in (switch off: the sampler-scored variant, the black cube, ReflectAlpha 0)
  const Shot unbound = Shoot(*renderer, refl, rb, best_axis, best_sgn, true, true);    // the same variants as `on`, the cube never bound: the ONLY difference to `on` is the reflection itself
  const size_t cov = CountCovered(off.px), diff = CountDiff(on.px, unbound.px), diff_off = CountDiff(on.px, off.px);
  char d[240];
  std::snprintf(d, sizeof d, "camera %c%c, covered %zu px, reflection adds %zu px (%.1f %%); vs the switch-off picture %zu px differ", best_sgn > 0 ? '+' : '-', "XYZ"[best_axis], cov, diff, cov ? 100.0 * double(diff) / double(cov) : 0.0, diff_off);
  Check(on.ok && off.ok && unbound.ok && cov > 200, "real: the reflective meshes draw (switch on, off, cube unbound)", d);
  Check(diff * 100 >= cov * 3, "B1 real: the reflective surfaces GAIN the reflection (cube bound != the same draw with the cube unbound, >= 3 % of the covered pixels)", d);
  std::printf("[rfl_unit] %s\n", on.line.c_str());
  Check(on.cubes >= 1 && on.draws > 0 && on.fallbacks == 0, "B2 real: the census line reports the cube loaded, reflect draws > 0 and no fallback", on.line);
  // nothing non-reflective changes: the control draws the same with the switch on and off
  const Shot con = Shoot(*renderer, ctrl, cb, 1, 1.f, true, g_plant), coff = Shoot(*renderer, ctrl, cb, 1, 1.f, false, false);
  const size_t ccov = con.ok ? CountCovered(con.px) : 0;
  std::snprintf(d, sizeof d, "control covered %zu px, differing %zu px", ccov, con.ok && coff.ok ? CountDiff(con.px, coff.px, 0) : size_t(0));
  Check(con.ok && coff.ok && ccov > 100 && CountDiff(con.px, coff.px, 0) == 0, "B3 real: a non-reflective model (yBG_Default) is byte-identical with the switch on and off", d);
  (void)sheet;
}

}  // namespace

int main(int argc, char** argv) {
  std::string game, sheet;
  bool expect_fail = false;
  int arena = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--plant" && i + 1 < argc) { const std::string pm = argv[++i]; g_plant = pm == "unbound"; g_noflip = pm == "noflip"; }
    else if (a == "--expect-fail") expect_fail = true;
    else if (a == "--arena" && i + 1 < argc) arena = std::atoi(argv[++i]);
    else if (a == "--sheet" && i + 1 < argc) sheet = argv[++i];
    else return std::fprintf(stderr, "usage: rfl_unit --game DIR [--plant unbound] [--expect-fail] [--arena N]\n"), 2;
  }
  if (game.empty()) return std::fprintf(stderr, "usage: rfl_unit --game DIR [--plant unbound] [--expect-fail] [--arena N]\n"), 2;
  if (const char* mc = std::getenv("RDR_MAIN_CHECKOUT")) {   // a check row names game_data under the MAIN checkout (a worktree has none)
    if (!std::filesystem::exists(game)) game = (std::filesystem::path(mc) / game).string();
  }
  launcher::GameFiles files;
  files.SetRoot(game);
  if (!BuildTable(files)) return 2;
  g_reflections.store(true);
  TestFaces(false);
  TestFaces(true);   // the live record frame draws in the viewer's space (record_frame.cpp: viewer_space = true): +Y / -Y must not swap
  TestReal(files, arena, sheet);
  if (expect_fail) {
    std::printf(g_bad ? "planted twin CAUGHT (%d check(s) failed): PASS\n" : "planted twin NOT caught: FAIL\n", g_bad);
    return g_bad ? 0 : 1;
  }
  std::printf(g_bad ? "FAIL: %d check(s)\n" : "PASS\n", g_bad);
  return g_bad ? 1 : 0;
}
