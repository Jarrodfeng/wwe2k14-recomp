// body-scale: none (front-end unit, no wrestler is drawn)  [S103 RGF: body_scale_feed_lint.py]
// S103 TQ2 fix F3 unit: the SearchLight YZ planar uv in game space and in the viewer's y-negated space, drawn through the REAL FrontEnd (AddModel + RenderToPixels, a headless Vulkan renderer; 0 game runs, no game files).
//   tq2_unit [--plant noflip] [--expect-fail]
// A yBG_UVScroll technique-10 (SearchLight_YZ) quad in the y-z plane, a texture that changes along both uv axes, SL_Scale 1, scroll speeds 0. The same quad is drawn twice from the same side: in game space (the vertices as
// the game has them) and in the viewer's space (every y negated: what the live record frame draws in, SceneFrame::viewer_space). The vertex shader projects world.yz; in viewer space world.y is negated, so Resolve feeds
// SL_Scale.y = -1 (the RFL lesson again) and both pictures must be the SAME. Checks: the picture is not flat (the uv really varies across the quad: the planar pair is bound), game == viewer (0 pixels differ),
// the SearchLight (switch on) picture differs from the switch-off one (the old one-layer, constant-uv draw), 0 validation errors.
// --plant noflip = FrontEndOptions::plant_no_mirror (the viewer-space draw is told it is game space: SL_Scale.y = +1): the viewer picture is mirrored and the check game == viewer must FAIL; --expect-fail inverts the exit code.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/record_io.h"

using namespace rdr_scene;

namespace {

constexpr uint32_t W = 64, H = 64;
int g_bad = 0;
bool g_noflip = false;

void Check(bool ok, const std::string& what, const std::string& detail = std::string()) {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  ", detail.c_str());
  if (!ok) ++g_bad;
}

assets::Texture Gradient(const char* name) {   // 8x8 RGBA: red = column, green = row, blue 128: every uv position has its own colour
  assets::Texture t;
  t.name = name, t.format = assets::TexFormat::kR8G8B8A8, t.width = t.height = 8, t.mips = 1;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      t.data.push_back(uint8_t(x * 36)), t.data.push_back(uint8_t(y * 36)), t.data.push_back(128), t.data.push_back(255);
    }
  return t;
}

// A y-z quad at x = 0, 1.6 across, normal along +X. `flip_y` = the viewer's space (every y negated).
assets::Model Quad(bool flip_y) {
  assets::Model m;
  m.label = "tq2_searchlight";
  m.textures.push_back(Gradient("sl_diffuse"));
  m.textures.push_back(Gradient("sl_layer2"));
  assets::Material mat;
  mat.shader = "yBG_UVScroll";
  mat.technique = 10;   // SearchLight_YZ
  mat.diffuse = 0, mat.layer[0] = 1;
  mat.sl_scale = 1.f;
  mat.scroll_have = true;   // speeds 0: a still texture
  for (int c = 0; c < 4; ++c) mat.ambient[c] = 1.f, mat.diffuse_color[c] = 1.f;
  m.materials.push_back(mat);
  assets::Mesh me;
  me.part = "quad";
  me.material = 0;
  const float sy[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
  for (int i = 0; i < 4; ++i) {
    assets::Vertex vx{};
    vx.pos[0] = 0.f, vx.pos[1] = 0.8f * sy[i] * (flip_y ? -1.f : 1.f), vx.pos[2] = 0.8f * sz[i];
    vx.normal[0] = -1.f;
    vx.color = 0xFFFFFFFFu;
    vx.weights[0] = 1.f;
    me.vertices.push_back(vx);
  }
  me.indices = {0, 1, 2, 0, 2, 3};
  m.meshes.push_back(me);
  return m;
}

// The camera looks down -X from +X; screen x = the game's y, screen y = z (viewer space: screen x = -y' so the picture is the same).
std::vector<uint8_t> Draw(rdr::Renderer& renderer, bool viewer, bool switch_on, uint32_t& val_errors) {
  g_table_pick_all.store(switch_on);
  FrontEndOptions fo;
  fo.plant_no_mirror = g_noflip;
  FrontEnd fe(renderer, fo);
  std::vector<std::string> warnings;
  const int id = fe.AddModel(Quad(viewer), warnings);
  SceneFrame sf;
  sf.width = W, sf.height = H;
  sf.viewer_space = viewer;
  sf.clear[0] = sf.clear[1] = sf.clear[2] = 0.f, sf.clear[3] = 1.f;
  Mat4 mvp = IdentityMat4();   // clip = [p 1] * M (row vectors): clip.x = game y (viewer: -y'), clip.y = z, clip.z = 0.5 - 0.125 x (depth), clip.w = 1
  const float ys = viewer ? -1.f : 1.f;
  mvp.m[0] = 0.f, mvp.m[5] = 0.f, mvp.m[10] = 0.f;
  mvp.m[1 * 4 + 0] = ys, mvp.m[2 * 4 + 1] = 1.f, mvp.m[0 * 4 + 2] = -0.125f, mvp.m[3 * 4 + 2] = 0.5f;
  sf.camera.view = mvp;
  sf.camera.proj = IdentityMat4();
  const float eye[3] = {3.f, 0.f, 0.f};
  std::memcpy(sf.camera.eye, eye, sizeof eye);
  Object ob;
  ob.asset = uint32_t(id);
  ob.world = IdentityMat4();
  sf.objects.push_back(ob);
  std::vector<uint8_t> px;
  std::string err;
  if (!fe.RenderToPixels(sf, px, err)) std::printf("render failed: %s\n", err.c_str());
  val_errors += fe.validation_errors();
  renderer.WaitIdle();
  return px;
}

size_t CountDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  size_t n = 0;
  for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
    for (int c = 0; c < 3; ++c)
      if (std::abs(int(a[i + size_t(c)]) - int(b[i + size_t(c)])) > 6) { ++n; break; }
  return n;
}

size_t Distinct(const std::vector<uint8_t>& a) {   // distinct colours among the covered pixels (a flat sheet has 1)
  std::vector<uint32_t> seen;
  for (size_t i = 0; i + 3 < a.size(); i += 4) {
    const uint32_t c = (uint32_t(a[i] >> 3) << 10) | (uint32_t(a[i + 1] >> 3) << 5) | uint32_t(a[i + 2] >> 3);
    if (!c) continue;
    bool f = false;
    for (uint32_t s : seen) f = f || s == c;
    if (!f) seen.push_back(c);
  }
  return seen.size();
}

}  // namespace

int main(int argc, char** argv) {
  bool expect_fail = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) g_noflip = std::string(argv[++i]) == "noflip";
    else if (a == "--expect-fail") expect_fail = true;
    else return std::fprintf(stderr, "usage: tq2_unit [--plant noflip] [--expect-fail]\n"), 2;
  }
  std::string err;
  rdr::RendererOptions ro;
  ro.validation = 1;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) return std::fprintf(stderr, "no headless renderer: %s\n", err.c_str()), 2;
  uint32_t val = 0;
  const auto game = Draw(*renderer, false, true, val);
  const auto view = Draw(*renderer, true, true, val);
  const auto off = Draw(*renderer, false, false, val);
  const size_t d = Distinct(game);
  Check(d >= 8, "the SearchLight YZ quad (switch on) shows a varying texture (the planar uv is bound)", std::to_string(d) + " distinct colours");
  const size_t gv = CountDiff(game, view);
  Check(gv == 0, "game space == viewer space (SL_Scale.y flips world.y back)", std::to_string(gv) + " pixels differ");
  const size_t on_off = CountDiff(game, off);
  Check(on_off > 100, "the switch on differs from the switch off (the old one-layer constant-uv sheet)", std::to_string(on_off) + " pixels differ");
  Check(val == 0, "0 validation errors", std::to_string(val));
  if (expect_fail) {
    std::printf(g_bad ? "planted twin CAUGHT (%d check(s) failed): PASS\n" : "planted twin NOT caught: FAIL\n", g_bad);
    return g_bad ? 0 : 1;
  }
  std::printf(g_bad ? "FAIL: %d check(s)\n" : "PASS\n", g_bad);
  return g_bad ? 1 : 0;
}
