// S111 CARDCULL unit (review): the far crowd card's face cull against the game's, through both vertex chains. 0 game runs (docs\research\s111\CARDCULL.md).
//   out\tools\cardcull_unit.exe --src DIR    DIR = the checkout's src folder.
// The card: the game's quad (src\renderer_scene\crowd_card_quad.h far_card, = sub_82507890's four vertices) placed by a member world as sub_8250CA78 builds it
// (a y turn from the member's angle, x / z rows scaled by the member scale, the member position), seen by perspective cameras in front of the card, behind
// it, oblique, from above, and through a mirrored view.
//   The GAME's chain: the strip 0, 1, 2, 3 (primitive 6, D3D's strip order), clip = p W VP, D3D's window (y down), the crowd node's device cull mode 6 (cull back,
//   front clockwise: the node turns the cull on and sets no winding; the stream's card draws carry it, translated: cull 2, front 1).
//   OURS: far_card::kIndices, the same clip (the card's vertices, its world rows and the view-projection are the record's, pushed as they are: DrawInstanced's
//   instance rows and its ViewProj rows), the wrapped shader's flip (ndc_scale = DrawInstanced's dynamic.system (1, -1, 1)), Vulkan's viewport (y down), and
//   DrawInstanced's own state (cull 2, or 0 under the planted InstancedDraw::plant_two_sided; front_face 1). S111 CROWDCULLSRC: not a material's flag.
// Checks: every triangle kept / dropped alike for every camera; one side of the card drawn whole, the other dropped whole. Planted twins that must fail: the
// rig fault cards_two_sided (plant_two_sided: both sides drawn), the other front face, and no flip. The source check ties the modelled state to DrawInstanced's
// lines (and that it reads no Material::cull_back), the card build to far_card, and the two-sided plant to the rig faults (a change there fails here until the model is updated).
// Build + run: pwsh -NoProfile -File tools\scene\build_cardcull_unit.ps1
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../../src/renderer_scene/caster_cull.h"
#include "../../src/renderer_scene/crowd_card_quad.h"

using namespace rdr_scene;
namespace fc = rdr_scene::live::far_card;

namespace {

int g_bad = 0;
void Check(bool ok, const std::string& name, const std::string& note = "") {
  std::printf("%s  %-46s %s\n", ok ? "PASS" : "FAIL", name.c_str(), note.c_str());
  g_bad += ok ? 0 : 1;
}

struct V3 {
  float x, y, z;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Norm(V3 a) {
  const float l = std::sqrt(Dot(a, a));
  return l > 0.f ? V3{a.x / l, a.y / l, a.z / l} : a;
}

// Row-vector 4x4 (m[4 * i + k] = row i, column k; p' = p M), the game's and the record's layout.
using M4 = std::array<float, 16>;
M4 Mul(const M4& a, const M4& b) {
  M4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r[size_t(4 * i + j)] += a[size_t(4 * i + k)] * b[size_t(4 * k + j)];
  return r;
}
std::array<float, 4> Apply4(const M4& m, V3 p) {
  std::array<float, 4> o{};
  for (int k = 0; k < 4; ++k) o[size_t(k)] = p.x * m[size_t(k)] + p.y * m[size_t(4 + k)] + p.z * m[size_t(8 + k)] + m[size_t(12 + k)];
  return o;
}
V3 Apply(const M4& m, V3 p) {
  const auto o = Apply4(m, p);
  return {o[0], o[1], o[2]};
}

// sub_8250CA78: a y turn (a proper rotation), x / z rows scaled by the member scale, row (1, 1) = the scale, the member position as translation.
M4 MemberWorld(float deg, float scale, V3 pos) {
  const float r = deg * 3.14159265f / 180.f, cs = std::cos(r), sn = std::sin(r);
  return {cs * scale, 0, -sn * scale, 0, 0, scale, 0, 0, sn * scale, 0, cs * scale, 0, pos.x, pos.y, pos.z, 1};
}

// A camera: D3DXMatrixLookAtLH's axes (mirror = -1 negates the view's x axis: an improper view, to show the agreement does not rest on the camera's handedness)
// times a D3D-style perspective (clip w = view z).
M4 Camera(V3 eye, V3 at, V3 up_hint, float mirror) {
  const V3 z = Norm(Sub(at, eye)), x = Scale(Norm(Cross(up_hint, z)), mirror), y = Cross(z, Scale(x, mirror));
  const M4 view = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1};
  const float ys = 1.f / std::tan(0.5f * 0.8f), xs = ys / (16.f / 9.f), zn = 10.f, zf = 100000.f, a = zf / (zf - zn);
  const M4 proj = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, a, 1, 0, 0, -zn * a, 0};
  return Mul(view, proj);
}

struct P2 {
  float x, y;
};
constexpr float kW = 1280.f, kH = 720.f;
// The game: D3D's window, y down.
P2 GameWindow(const std::array<float, 4>& c) { return {(c[0] / c[3] + 1.f) * 0.5f * kW, (1.f - c[1] / c[3]) * 0.5f * kH}; }
// Ours: the wrapped shader's flip (gl_Position.xyz * ndc_scale), then Vulkan's viewport (y down, positive height: DrawInstanced's dynamic.viewport).
P2 OursFramebuffer(const std::array<float, 4>& c, float ndc_scale_y) { return {(c[0] / c[3] + 1.f) * 0.5f * kW, (ndc_scale_y * c[1] / c[3] + 1.f) * 0.5f * kH}; }
float WindowCross(P2 a, P2 b, P2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }   // > 0: clockwise on the screen (y down)
// The device's cull mode: bit 0 cull front, bit 1 cull back, bit 2 front = clockwise.
bool GameDrops(uint32_t mode, P2 a, P2 b, P2 c) {
  const bool cw = WindowCross(a, b, c) > 0.f;
  const bool front = (mode & 4u) ? cw : !cw;
  return (mode & (front ? 1u : 2u)) != 0;
}
constexpr uint32_t kCrowdNodeMode = 6;   // cull back, front clockwise (the reset's winding 2 under the node's cull-on)

// DrawInstanced's state (front_end.cpp FrontEnd::DrawInstanced: state.cull = d.plant_two_sided ? 0 : 2; state.front_face = 1; the source check below).
CasterFace InstancedFace(bool plant_two_sided) { return CasterFace{uint8_t(plant_two_sided ? 0 : 2), 1}; }

struct Ours {
  CasterFace face;
  float ndc_scale_y;
};

struct View {
  const char* name;
  M4 vp;
  bool facing_plus;   // the eye is on the card's +z side (after the member's turn)
};

struct Result {
  int disagree = 0, tris = 0;
  int drawn_plus = 0, drawn_minus = 0, dropped_plus = 0, dropped_minus = 0;   // triangles, by the side the eye is on
  std::string note;
};

Result Run(const View* views, int nviews, const M4& world, Ours ours) {
  Result r;
  V3 p[4];
  for (int i = 0; i < 4; ++i) p[i] = Apply(world, V3{fc::kX[i], fc::kY[i], 0.f});
  // The game's strip, expanded with D3D's rule: triangle i of indices 0, 1, 2, 3 is (i, i+1, i+2) for even i, (i+1, i, i+2) for odd i.
  const uint32_t strip[4] = {0, 1, 2, 3};
  for (int v = 0; v < nviews; ++v) {
    const View& vw = views[v];
    int drawn = 0;
    for (int t = 0; t < 2; ++t) {
      const uint32_t g0 = t % 2 ? strip[t + 1] : strip[t], g1 = t % 2 ? strip[t] : strip[t + 1], g2 = strip[t + 2];
      const P2 gw[3] = {GameWindow(Apply4(vw.vp, p[g0])), GameWindow(Apply4(vw.vp, p[g1])), GameWindow(Apply4(vw.vp, p[g2]))};
      const uint32_t* oi = &fc::kIndices[3 * t];
      P2 ow[3];
      for (int k = 0; k < 3; ++k) {
        const V3 q = Apply(world, V3{fc::kX[oi[k]], fc::kY[oi[k]], 0.f});
        ow[k] = OursFramebuffer(Apply4(vw.vp, q), ours.ndc_scale_y);
      }
      const float pa[2] = {ow[0].x, ow[0].y}, pb[2] = {ow[1].x, ow[1].y}, pc[2] = {ow[2].x, ow[2].y};
      const bool gd = GameDrops(kCrowdNodeMode, gw[0], gw[1], gw[2]);
      const bool od = CasterCulled(ours.face, pa, pb, pc);
      ++r.tris;
      r.disagree += gd != od ? 1 : 0;
      drawn += od ? 0 : 1;
      (vw.facing_plus ? (od ? r.dropped_plus : r.drawn_plus) : (od ? r.dropped_minus : r.drawn_minus)) += 1;
    }
    r.note += std::string(vw.name) + " " + std::to_string(drawn) + "/2; ";
  }
  return r;
}

bool ReadText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream s;
  s << f.rdbuf();
  out = s.str();
  return true;
}

// The modelled state is DrawInstanced's and the card build reads far_card: a change in either fails here until this unit's model is brought along.
void SourceCheck(const std::string& src) {
  std::string fe, lc;
  const bool have = ReadText(src + "/renderer_scene/front_end.cpp", fe) && ReadText(src + "/renderer_scene/live_crowd.cpp", lc);
  Check(have, "source.read", have ? "" : "cannot read front_end.cpp / live_crowd.cpp under " + src);
  if (!have) return;
  const size_t fn = fe.find("uint32_t FrontEnd::DrawInstanced(");
  const size_t cull = fn == std::string::npos ? fn : fe.find("state.cull = ", fn);
  const size_t depth = cull == std::string::npos ? cull : fe.find("state.depth_test", cull);
  const std::string block = depth == std::string::npos ? std::string() : fe.substr(cull, depth - cull);
  Check(block.rfind("state.cull = d.plant_two_sided ? 0 : 2;", 0) == 0 && block.find("state.front_face = 1;") != std::string::npos, "source.drawinstanced_state",
        "FrontEnd::DrawInstanced: state.cull = d.plant_two_sided ? 0 : 2; state.front_face = 1 (the state InstancedFace models)");
  // S111 CROWDCULLSRC: the cull is the draw's own (every instanced draw is a crowd draw, under the crowd node's cull-on), never a material's flag.
  const size_t fn_end = fn == std::string::npos ? fn : fe.find("\n}", fn);   // (the closing brace at column 0; the files are CRLF)
  const std::string body = fn_end == std::string::npos ? std::string() : fe.substr(fn, fn_end - fn);
  Check(!body.empty() && body.find("cull_back") == std::string::npos, "source.drawinstanced_no_material_cull",
        "FrontEnd::DrawInstanced reads no Material::cull_back (the generic model draw's rule; no crowd draw goes through it)");
  const bool cards = lc.find("far_card::kIndices") != std::string::npos && lc.find("far_card::kX[") != std::string::npos && lc.find("far_card::kY[") != std::string::npos;
  Check(cards, "source.card_build_reads_far_card", "LiveCrowd's card quad: far_card's vertices and indices");
  const std::string store = "plant_two_sided = ", from_fault = "plant_two_sided = s.options.fault_";
  int stores = 0, faults = 0;
  for (size_t p = lc.find(store); p != std::string::npos; p = lc.find(store, p + 1)) ++stores, faults += lc.compare(p, from_fault.size(), from_fault) == 0 ? 1 : 0;
  const bool card_twin = lc.find("d.plant_two_sided = s.options.fault_cards_two_sided;") != std::string::npos;
  Check(card_twin && stores == faults, "source.two_sided_only_planted",
        "LiveCrowd: " + std::to_string(stores) + " plant_two_sided stores, " + std::to_string(faults) + " from a rig fault option; the cards' twin " + (card_twin ? "present" : "MISSING"));
}

}  // namespace

int main(int argc, char** argv) {
  std::string src;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--src") src = argv[i + 1];
  if (src.empty()) {
    std::printf("usage: cardcull_unit --src DIR (the checkout's src folder)\n");
    return 2;
  }
  SourceCheck(src);

  // A member turned 35 degrees at scale 1.1 (sub_8250CA78), the card's centre 125 along -y (the quad spans y 0 .. -250).
  const V3 pos{400.f, 20.f, -900.f};
  const M4 world = MemberWorld(35.f, 1.1f, pos);
  const V3 centre = Apply(world, {0.f, -125.f, 0.f});
  const V3 n = Norm(V3{world[8], world[9], world[10]});   // the card's +z after the turn
  const V3 side = Norm(V3{world[0], world[1], world[2]});
  const V3 up{0.f, -1.f, 0.f};   // the file's up is -y
  const View views[] = {
      {"+z", Camera(Add(centre, Scale(n, 3000.f)), centre, up, 1.f), true},
      {"-z", Camera(Sub(centre, Scale(n, 3000.f)), centre, up, 1.f), false},
      {"+z oblique", Camera(Add(Add(centre, Scale(n, 2000.f)), Scale(side, 1500.f)), centre, up, 1.f), true},
      {"-z oblique", Camera(Add(Sub(centre, Scale(n, 2000.f)), Scale(side, -1200.f)), centre, up, 1.f), false},
      {"+z above", Camera(Add(Add(centre, Scale(n, 2500.f)), Scale(up, 1800.f)), centre, up, 1.f), true},
      {"-z above", Camera(Add(Sub(centre, Scale(n, 2500.f)), Scale(up, 1800.f)), centre, up, 1.f), false},
      {"+z mirrored view", Camera(Add(centre, Scale(n, 3000.f)), centre, up, -1.f), true},
      {"-z mirrored view", Camera(Sub(centre, Scale(n, 3000.f)), centre, up, -1.f), false},
  };
  const int nv = int(sizeof views / sizeof views[0]);
  const int proper = 6;   // the first six views are proper (LookAtLH); the mirrored pair is checked for agreement only

  const Ours prod{InstancedFace(false), -1.f};
  const Result r = Run(views, nv, world, prod);
  Check(r.disagree == 0, "card.faces_as_the_game", std::to_string(r.disagree) + " of " + std::to_string(r.tris) + " triangles kept / dropped otherwise (" + r.note + ")");
  const Result rp = Run(views, proper, world, prod);
  const bool one_side = (rp.drawn_plus == 6 && rp.dropped_minus == 6 && rp.drawn_minus == 0 && rp.dropped_plus == 0) ||
                        (rp.drawn_minus == 6 && rp.dropped_plus == 6 && rp.drawn_plus == 0 && rp.dropped_minus == 0);
  Check(one_side, "card.one_side_drawn_whole", "drawn from +z " + std::to_string(rp.drawn_plus) + "/6, from -z " + std::to_string(rp.drawn_minus) + "/6 triangles");
  std::printf("      (proper LookAtLH cameras: the side drawn is %s of the card's local +z after the member's turn)\n", rp.drawn_plus ? "the eye on the +z side" : "the eye on the -z side");

  // the planted twins
  const Result two = Run(views, nv, world, Ours{InstancedFace(true), -1.f});
  Check(two.disagree > 0 && (two.drawn_plus + two.drawn_minus) == two.tris, "twin.cards_two_sided_caught",
        "the rig fault's card draws its back too: " + std::to_string(two.disagree) + " triangles the game drops are drawn");
  const Result other = Run(views, nv, world, Ours{CasterFace{2, 0}, -1.f});
  Check(other.disagree == other.tris, "twin.other_front_face_caught", "front_face 0: " + std::to_string(other.disagree) + " of " + std::to_string(other.tris) + " triangles differ (the card facing the camera vanishes)");
  const Result noflip = Run(views, nv, world, Ours{InstancedFace(false), 1.f});
  Check(noflip.disagree == noflip.tris, "twin.no_flip_caught", "ndc_scale.y = +1: " + std::to_string(noflip.disagree) + " of " + std::to_string(noflip.tris) + " triangles differ");

  std::printf("%s cardcull_unit: the far card culls the faces the game's crowd node culls (device mode 6) through DrawInstanced's state\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}
