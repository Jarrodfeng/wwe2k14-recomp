// S111 CASTCULL unit: the caster passes' face cull (src\renderer_scene\caster_cull.h) against the game's device rule, through our vertex chain. 0 game runs
// (docs\research\s111\CASTCULL.md).
//   out\tools\castcull_unit.exe                   selftest: a closed box and an open card in the file's space, put through the GAME's chain (file vertex, the
//                                                  world, the light, D3D's window with y down, the device cull mode 6 / 2) and through OURS (the loader's Y
//                                                  mirror, the viewer-space world S W S, the pass's mirror row, system[1] = -1, Vulkan's face test with our
//                                                  CasterFace): every triangle is kept / dropped alike, the box casts the same picture culled or not, the card
//                                                  casts from one side only. The planted twins (cull off; the other face) must fail.
//   out\tools\castcull_unit.exe real --game DIR   read-only, the player's files: ch161 attire 0 through the production loader, the caster meshes CasterOf keeps
//                                                  (a character part, Material::char_part: S111 UNIFY5 review): per pass and light the triangles the cull drops (drawn before), the texels
//                                                  that stop casting, and every closed part casts the same picture.
// Build + run: pwsh -NoProfile -File tools\scene\build_castcull_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer_scene/caster_cull.h"
#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/wrestler_caster.h"   // S111 LAYERS review: the caster set (one predicate with CasterOf)

using namespace rdr_scene;

namespace {

int g_bad = 0;
void Check(bool ok, const char* name, const std::string& note = "") {
  std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, note.c_str());
  g_bad += ok ? 0 : 1;
}

struct V3 {
  float x, y, z;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Norm(V3 a) {
  const float l = std::sqrt(Dot(a, a));
  return l > 0.f ? V3{a.x / l, a.y / l, a.z / l} : a;
}

// A row-vector 4x4 in assets::Matrix's layout (m[4 * i + k] = row i, column k; p' = p M), as the record's world rows.
using M4 = std::array<float, 16>;
V3 Apply(const M4& m, V3 p) {
  return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13], p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}
M4 Mul(const M4& a, const M4& b) {
  M4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r[size_t(4 * i + j)] += a[size_t(4 * i + k)] * b[size_t(4 * k + j)];
  return r;
}
const M4 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
const M4 kMirrorY = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // the viewer's Y mirror S
M4 TurnY(float deg, V3 t) {
  const float r = deg * 3.14159265f / 180.f, cs = std::cos(r), sn = std::sin(r);
  return {cs, 0, -sn, 0, 0, 1, 0, 0, sn, 0, cs, 0, t.x, t.y, t.z, 1};
}

// An orthographic light with D3DXMatrixLookAtLH's axes (a proper rotation): ndc = (x, y) of the light's view over `half`. Both chains read the same game-space light.
struct Light {
  V3 ax, ay, az, c;
  float half;
  const char* name;
};
Light LookFrom(V3 forward, V3 up_hint, V3 c, float half, const char* name) {
  const V3 z = Norm(forward), x = Norm(Cross(up_hint, z)), y = Cross(z, x);
  return {x, y, z, c, half, name};
}

struct P2 {
  float x, y;
};
// The GAME's chain: the file vertex through the world, the light, D3D's window (y down).
P2 GameWindow(const Light& L, V3 world, int n) {
  const V3 d = Sub(world, L.c);
  const float nx = Dot(L.ax, d) / L.half, ny = Dot(L.ay, d) / L.half;
  return {(nx + 1.f) * 0.5f * float(n), (1.f - ny) * 0.5f * float(n)};
}
float WindowCross(P2 a, P2 b, P2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }   // > 0: clockwise as seen on the screen (y down)
// The device's cull mode: bit 0 cull front, bit 1 cull back, bit 2 front = clockwise.
bool GameDrops(uint32_t mode, P2 a, P2 b, P2 c) {
  const bool cw = WindowCross(a, b, c) > 0.f;
  const bool front = (mode & 4u) ? cw : !cw;
  return (mode & (front ? 1u : 2u)) != 0;
}
// OURS: the loader's vertex (FinishModel: y negated) through the viewer-space world rows (S W S), the pass's mirror row (the output's y negated:
// rows (k == 1 ? mirror : 1) * mt[i * 4 + k]), the same light, system[1] = -1 (ndc_scale.y), Vulkan's viewport (y down).
P2 OursFramebuffer(const Light& L, V3 loader_v, const M4& viewer_world, int n) {
  V3 p = Apply(viewer_world, loader_v);
  p.y = -p.y;
  const V3 d = Sub(p, L.c);
  const float cx = Dot(L.ax, d) / L.half, cy = Dot(L.ay, d) / L.half;
  const float ndc_y = cy * -1.f;
  return {(cx + 1.f) * 0.5f * float(n), (ndc_y + 1.f) * 0.5f * float(n)};
}

// Coverage at texel centres (either winding).
void Raster(P2 a, P2 b, P2 c, int n, std::vector<uint8_t>& cov) {
  const float area = WindowCross(a, b, c);
  if (area == 0.f) return;
  const float s = area > 0.f ? 1.f : -1.f;
  const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(n - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
  const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(n - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const P2 p{float(x) + 0.5f, float(y) + 0.5f};
      if (s * WindowCross(a, b, p) >= 0.f && s * WindowCross(b, c, p) >= 0.f && s * WindowCross(c, a, p) >= 0.f) cov[size_t(y) * size_t(n) + size_t(x)] = 1;
    }
}
size_t Count(const std::vector<uint8_t>& v) { return size_t(std::count(v.begin(), v.end(), uint8_t(1))); }

struct Tri {
  V3 f[3];   // the file's (the game's) space
  int part;
};

struct PassRun {
  int tris = 0, flat = 0, dropped = 0, disagree = 0;
  std::vector<uint8_t> all, kept;   // texels covered with the cull off / on
};

PassRun Run(const std::vector<Tri>& tris, const M4& game_world, const Light& L, uint32_t mode, CasterFace face, int n, int only_part = -1) {
  PassRun r;
  r.all.assign(size_t(n) * size_t(n), 0), r.kept.assign(size_t(n) * size_t(n), 0);
  const M4 viewer_world = Mul(Mul(kMirrorY, game_world), kMirrorY);   // BuildFrame's viewer-space world
  for (const Tri& t : tris) {
    if (only_part >= 0 && t.part != only_part) continue;
    P2 g[3], o[3];
    for (int k = 0; k < 3; ++k) {
      g[k] = GameWindow(L, Apply(game_world, t.f[k]), n);
      o[k] = OursFramebuffer(L, V3{t.f[k].x, -t.f[k].y, t.f[k].z}, viewer_world, n);
    }
    ++r.tris;
    if (std::fabs(WindowCross(g[0], g[1], g[2])) < 1e-2f) {   // edge-on (area < 1/200 texel): covers no texel either way, its sign is float noise
      ++r.flat;
      continue;
    }
    const float pa[2] = {o[0].x, o[0].y}, pb[2] = {o[1].x, o[1].y}, pc[2] = {o[2].x, o[2].y};
    const bool gd = GameDrops(mode, g[0], g[1], g[2]);
    const bool od = CasterCulled(face, pa, pb, pc);
    r.disagree += gd != od ? 1 : 0;
    r.dropped += od ? 1 : 0;
    Raster(o[0], o[1], o[2], n, r.all);
    if (!od) Raster(o[0], o[1], o[2], n, r.kept);
  }
  return r;
}

// Welded (1/100 unit) edges used by one triangle only: 0 = a closed part.
int OpenEdges(const std::vector<Tri>& tris, int part) {
  std::map<std::array<long long, 3>, int> ids;
  std::map<std::pair<int, int>, int> edges;
  const auto id = [&](V3 p) {
    const std::array<long long, 3> k{std::llround(double(p.x) * 100.0), std::llround(double(p.y) * 100.0), std::llround(double(p.z) * 100.0)};
    return ids.emplace(k, int(ids.size())).first->second;
  };
  for (const Tri& t : tris) {
    if (part >= 0 && t.part != part) continue;
    const int v[3] = {id(t.f[0]), id(t.f[1]), id(t.f[2])};
    for (int e = 0; e < 3; ++e) {
      const int a = v[e], b = v[(e + 1) % 3];
      if (a != b) ++edges[{std::min(a, b), std::max(a, b)}];
    }
  }
  int open = 0;
  for (const auto& e : edges) open += e.second == 1 ? 1 : 0;
  return open;
}

void AddQuad(std::vector<Tri>& out, V3 a, V3 b, V3 c, V3 d, int part) {
  out.push_back({{a, b, c}, part});
  out.push_back({{a, c, d}, part});
}
std::vector<Tri> Box(V3 c, float h) {   // twelve triangles, every one wound the same way round its outward normal
  std::vector<Tri> out;
  for (int ax = 0; ax < 3; ++ax)
    for (int s = -1; s <= 1; s += 2) {
      float n[3] = {0, 0, 0}, u[3] = {0, 0, 0}, v[3] = {0, 0, 0};
      n[ax] = float(s), u[(ax + 1) % 3] = 1.f, v[(ax + 2) % 3] = 1.f;
      if (s < 0) std::swap(u, v);
      const auto corner = [&](float su, float sv) {
        return V3{c.x + h * (n[0] + su * u[0] + sv * v[0]), c.y + h * (n[1] + su * u[1] + sv * v[1]), c.z + h * (n[2] + su * u[2] + sv * v[2])};
      };
      AddQuad(out, corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1), 0);
    }
  return out;
}
std::vector<Tri> Card(V3 c, float h) {   // one open card in the plane z = c.z, facing +z
  std::vector<Tri> out;
  AddQuad(out, {c.x - h, c.y - h, c.z}, {c.x + h, c.y - h, c.z}, {c.x + h, c.y + h, c.z}, {c.x - h, c.y + h, c.z}, 0);
  return out;
}

// One face rule over the synthetic scene (a turned, moved world; lights in front of the card, behind it, and oblique).
struct Props {
  int disagree = 0;
  bool box_same = true, card_one_side = true;
  std::string note;
};
Props Evaluate(uint32_t mode, CasterFace face) {
  const int n = 64;
  const M4 w = TurnY(30.f, {10.f, -50.f, 20.f});
  const V3 centre = Apply(w, {0.f, -100.f, 0.f});
  const V3 normal{w[8], w[9], w[10]};   // the card's +z after the world's turn
  const V3 up{0.f, -1.f, 0.f};          // the file's up is -Y
  const Light front = LookFrom({-normal.x, -normal.y, -normal.z}, up, centre, 40.f, "front");
  const Light back = LookFrom(normal, up, centre, 40.f, "back");
  const Light oblique = LookFrom({-normal.x + 0.6f, 0.7f, -normal.z}, up, centre, 40.f, "oblique");
  const std::vector<Tri> box = Box({0.f, -100.f, 0.f}, 15.f), card = Card({0.f, -100.f, 0.f}, 15.f);
  Props p;
  for (const Light* L : {&front, &back, &oblique}) {
    const PassRun b = Run(box, w, *L, mode, face, n);
    p.disagree += b.disagree;
    if (b.all != b.kept || Count(b.all) == 0) p.box_same = false;
    p.note += std::string(L->name) + ": box " + std::to_string(Count(b.kept)) + "/" + std::to_string(Count(b.all));
    const PassRun c = Run(card, w, *L, mode, face, n);
    p.disagree += c.disagree;
    p.note += ", card " + std::to_string(Count(c.kept)) + "/" + std::to_string(Count(c.all)) + "; ";
  }
  const PassRun cf = Run(card, w, front, mode, face, n), cb = Run(card, w, back, mode, face, n);
  const size_t kf = Count(cf.kept), kb = Count(cb.kept);
  p.card_one_side = Count(cf.all) > 0 && Count(cb.all) > 0 && ((kf > 0) != (kb > 0)) && (kf == 0 || cf.kept == cf.all) && (kb == 0 || cb.kept == cb.all);
  return p;
}

struct Pass {
  const char* name;
  uint32_t mode;   // the game's device cull mode
  CasterFace face;   // ours
};
const Pass kPasses[] = {{"shadow", 6u, CasterFace{kCasterCull, kCasterFrontShadow}}, {"plane", 2u, CasterFace{kCasterCull, kCasterFrontPlane}}};

// S111 LAYERS review: the caster set is the game's per pass (wrestler_caster.h), keyed on the draw node, not on alpha_blend. Fixtures: a stock piece's cloth (translucent node,
// blended), a body skin part the front end made blended for a texel cut-out (opaque node, alpha_blend set), a created character's opaque-layer trans part (opaque node, not blended).
void CheckCasterSet() {
  namespace wc = wrestler_caster;
  assets::Material piece, cut, body;
  piece.shader = "yCh_cloth", piece.alpha_blend = piece.translucent_draw = true;
  cut.shader = "yCh_skin", cut.alpha_blend = true, cut.translucent_draw = false;
  body.shader = "yCh_trans", body.alpha_blend = body.translucent_draw = false;
  piece.char_part = cut.char_part = body.char_part = true;   // S111 UNIFY5 review: character parts (both builders set it)
  const bool match_b = wc::WalksTranslucent(wc::Pass::MapB, true, 1), entrance_b = wc::WalksTranslucent(wc::Pass::MapB, true, 4), rim = wc::WalksTranslucent(wc::Pass::Rim, false, 1);
  const bool a = wc::WalksTranslucent(wc::Pass::MapA, true, 4), mode1 = wc::WalksTranslucent(wc::Pass::MapB, false, 4), unknown = wc::WalksTranslucent(wc::Pass::MapB, true, -1);
  Check(!match_b && entrance_b && rim && !a && !mode1 && !unknown, "caster_set.passes_walk_category_33", "map B walks it unless the scene word is 1; the rim map always; map A never");
  Check(!wc::Casts(piece, match_b) && wc::Casts(piece, entrance_b) && wc::Casts(piece, rim), "caster_set.stock_piece", "a stock piece casts in an entrance's map B and the rim map, not in a match's map B");
  Check(wc::Casts(cut, match_b) && wc::Casts(body, match_b) && wc::Casts(body, false), "caster_set.opaque_node_parts", "the opaque node's parts cast in every pass, alpha_blend or not");
  assets::Material prop;
  prop.shader = "yProp_BumpMap";
  Check(!wc::Casts(prop, true), "caster_set.only_wrestler_parts");
  // S111 PIECEROUTE review: a pieces-only owner (kObjPiecesOnly: the game drew no body, its translucent node drew the SUBM pieces) casts its piece meshes in the passes that walk that node
  // (map B unless the scene word is 1, the rim map), never its body, and nothing in map A. The planted twin is the rule before: only a visible object casts.
  Check(wc::ObjectMayCast(true, false, false) && wc::ObjectMayCast(false, true, entrance_b) && wc::ObjectMayCast(false, true, rim) && !wc::ObjectMayCast(false, true, match_b) &&
            !wc::ObjectMayCast(false, true, a) && !wc::ObjectMayCast(false, false, true),
        "caster_set.pieces_only_passes", "a pieces-only owner casts in an entrance's map B and the rim map, not in a match's map B or map A; a hidden body never");
  Check(wc::MeshCastsFor(true, false, false) && wc::MeshCastsFor(true, false, true) && wc::MeshCastsFor(false, true, true) && !wc::MeshCastsFor(false, true, false) && !wc::MeshCastsFor(false, false, true),
        "caster_set.pieces_only_meshes", "a visible body casts every caster mesh; a pieces-only owner its piece meshes only, never its body");
  const auto visible_only = [](bool visible, bool, bool) { return visible; };   // the planted twin: the rule before (every caster loop tested kObjVisible alone)
  Check(visible_only(false, true, rim) != wc::ObjectMayCast(false, true, rim), "twin.visible_only_caught", "the visible-only rule casts no piece of a pieces-only owner into the rim map");
  // S111 UNIFY5 review: the set is the character's parts, by Material::char_part, not by the effect name: a created superstar's yDefault part (char_part, opaque node) casts in map A, map B and the rim
  // map exactly as its yCh_skin part does; a stock yDefault arena mesh (no char_part) casts in none. The planted twin is the old key (a yCh_ effect).
  assets::Material gdef, gskin, adef;
  gdef.shader = "yDefault", gdef.char_part = true;
  gskin.shader = "yCh_skin", gskin.char_part = true;
  adef.shader = "yDefault";
  const bool same = wc::Casts(gdef, a) == wc::Casts(gskin, a) && wc::Casts(gdef, match_b) == wc::Casts(gskin, match_b) && wc::Casts(gdef, entrance_b) == wc::Casts(gskin, entrance_b) &&
                    wc::Casts(gdef, rim) == wc::Casts(gskin, rim);
  Check(same && wc::Casts(gdef, a) && wc::Casts(gdef, match_b) && wc::Casts(gdef, rim) && !wc::Casts(adef, a) && !wc::Casts(adef, rim), "caster_set.guest_ydefault_part",
        "a created superstar's yDefault part casts in map A / map B / the rim map like its yCh_skin part; an arena yDefault mesh does not");
  const auto effect_key = [](const assets::Material& m, bool walks) { return IsCharEffect(m.shader) && (walks || !m.translucent_draw); };   // the planted twin: the key before (the effect name)
  Check(!effect_key(gdef, a) && !effect_key(gdef, rim), "twin.effect_name_key_caught", "the effect-name key drops the guest yDefault part from every caster pass");
  const auto old_key = [](const assets::Material& m) { return IsCharEffect(m.shader) && !m.alpha_blend; };   // the planted twin: the key before (alpha_blend, one set for every pass)
  Check(old_key(cut) != wc::Casts(cut, match_b) && old_key(piece) != wc::Casts(piece, rim), "twin.alpha_blend_key_caught", "the alpha_blend key drops the cut-out skin and the rim map's piece");
}

int Selftest() {
  CheckCasterSet();
  for (const Pass& ps : kPasses) {
    const Props p = Evaluate(ps.mode, ps.face);
    const std::string name = std::string(ps.name) + " mode " + std::to_string(ps.mode);
    Check(p.disagree == 0, (name + ": faces as the game's").c_str(), std::to_string(p.disagree) + " triangles kept / dropped otherwise");
    Check(p.box_same, (name + ": closed box, same picture").c_str(), p.note);
    Check(p.card_one_side, (name + ": open card, one side").c_str());
  }
  // the planted twins: the old state (cull off) and the other face
  const Props off = Evaluate(6u, CasterFace{0, 1});
  Check(!off.card_one_side, "twin.cull_off_caught", "cull off: the card casts from both sides (" + off.note + ")");
  const Props wrong_shadow = Evaluate(6u, CasterFace{kCasterCull, uint8_t(1 - kCasterFrontShadow)});
  Check(wrong_shadow.disagree > 0, "twin.shadow_other_face_caught", std::to_string(wrong_shadow.disagree) + " triangles differ");
  const Props wrong_plane = Evaluate(2u, CasterFace{kCasterCull, uint8_t(1 - kCasterFrontPlane)});
  Check(wrong_plane.disagree > 0, "twin.plane_other_face_caught", std::to_string(wrong_plane.disagree) + " triangles differ");
  std::printf("%s castcull_unit selftest: the caster passes cull the faces the game's device modes 6 / 2 cull\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

// The player's files, read-only: ch161 attire 0 through the production loader, the bind pose (world = identity).
int Real(const char* game) {
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  assets::Model m;
  std::string err;
  const bool ok = assets::LoadWrestler(files, 161, 0, m, err);
  Check(ok && !m.meshes.empty(), "real.load_ch161", ok ? std::to_string(m.meshes.size()) + " meshes" : err);
  if (!ok) return 1;
  std::vector<Tri> tris;
  std::vector<std::string> parts;
  for (const assets::Mesh& me : m.meshes) {   // CasterOf's filter (live_map_shadow.cpp): valid indices, no crowd flash, a material, then wrestler_caster::Casts for map A / map B in a match (the opaque node's parts)
    if (me.vertices.empty() || me.indices.size() < 3) continue;
    bool bad = false;
    for (uint32_t ix : me.indices) bad = bad || ix >= me.vertices.size();
    if (bad) continue;
    if (me.part.rfind("crowd flash", 0) == 0) continue;
    if (me.material < 0 || size_t(me.material) >= m.materials.size()) continue;
    const assets::Material& mat = m.materials[size_t(me.material)];
    if (!wrestler_caster::Casts(mat, false)) continue;
    const int part = int(parts.size());
    parts.push_back(me.part + " (" + mat.shader + ")");
    for (size_t i = 0; i + 2 < me.indices.size(); i += 3) {
      Tri t;
      t.part = part;
      for (int k = 0; k < 3; ++k) {
        const assets::Vertex& v = me.vertices[me.indices[i + size_t(k)]];
        t.f[k] = V3{v.pos[0], -v.pos[1], v.pos[2]};   // FinishModel's Y mirror taken off: the file's (the game's) space
      }
      tris.push_back(t);
    }
  }
  Check(!tris.empty(), "real.casters", std::to_string(parts.size()) + " caster meshes, " + std::to_string(tris.size()) + " triangles");
  if (tris.empty()) return 1;
  V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (const Tri& t : tris)
    for (const V3& p : t.f) lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}, hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  const V3 c{0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)};
  const float half = 0.55f * std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
  const Light lights[] = {LookFrom({0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, c, half, "above"), LookFrom({0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, c, half, "below"),
                          LookFrom({0.f, 0.f, 1.f}, {0.f, -1.f, 0.f}, c, half, "+z"), LookFrom({0.f, 0.f, -1.f}, {0.f, -1.f, 0.f}, c, half, "-z"),
                          LookFrom({1.f, 0.f, 0.f}, {0.f, -1.f, 0.f}, c, half, "+x")};
  const int n = 256;
  int disagree = 0;
  for (const Pass& ps : kPasses)
    for (const Light& L : lights) {
      const PassRun r = Run(tris, kIdentity, L, ps.mode, ps.face, n);
      disagree += r.disagree;
      const size_t a = Count(r.all), k = Count(r.kept);
      std::printf("      %-6s light %-5s: %d triangles, %d dropped by the cull (drawn before), %d edge-on; texels %zu -> %zu (%zu stop casting)\n", ps.name, L.name, r.tris, r.dropped, r.flat, a, k, a - k);
    }
  Check(disagree == 0, "real.faces_match_game", std::to_string(disagree) + " triangles kept / dropped otherwise, 2 passes x 5 lights");
  int closed = 0, closed_differ = 0;
  std::printf("      per caster mesh (light above, shadow pass): triangles, open edges, dropped, texels that stop casting\n");
  for (int p = 0; p < int(parts.size()); ++p) {
    const int open = OpenEdges(tris, p);
    if (open == 0) {
      ++closed;
      for (const Pass& ps : kPasses)
        for (const Light& L : lights) {
          const PassRun r = Run(tris, kIdentity, L, ps.mode, ps.face, n, p);
          closed_differ += r.all != r.kept ? 1 : 0;
        }
    }
    const PassRun r = Run(tris, kIdentity, lights[0], kPasses[0].mode, kPasses[0].face, n, p);
    std::printf("      %-48s %6d %6d %6d %6zu\n", parts[size_t(p)].c_str(), r.tris, open, r.dropped, Count(r.all) - Count(r.kept));
  }
  Check(closed_differ == 0, "real.closed_parts_same_picture",
        std::to_string(closed) + " of " + std::to_string(parts.size()) + " caster meshes are closed; " + std::to_string(closed_differ) + " (mesh, pass, light) pictures differ");
  std::printf("%s castcull_unit real\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "real") {
    const char* game = nullptr;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--game") game = argv[i + 1];
    if (!game) {
      std::printf("usage: castcull_unit real --game DIR\n");
      return 2;
    }
    return Real(game);
  }
  return Selftest();
}
