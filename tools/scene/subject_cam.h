// S108 lane SHOT-B (docs\research\s108\SHOT_plan.md sections 3, 6, 7): the pure resolver of a subject shot, header only, no GPU, no game files.
//   subject (face / wrestler / stage / prop) -> a point + radius + facing -> a camera built in VIEWER space (y up) with wgs::MakeVP / PieceCamDir,
//   handed to the rig in the game's space by wgs::ToGame (the note's trap: a look-at built in game space with up = -y is a mirror, every inward face is culled);
//   and the CPU ray through a pixel with every triangle it crosses, front to back (the rig's --pick).
// Everything here takes VIEWER-space numbers (the SceneFrame BuildFrame made: objects, palettes, the camera). The game-space record is never read here.
// Row vectors (p' = p * M, translation in elements 12..14), as arena_cams.h.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "arena_cams.h"

namespace subj {

using wgs::V3;
using wgs::Cam;

// ---- the subject argument: face:0 | wrestler:1 | stage:114 | prop:2004 [,from=front|back|left|right|game] [,radius=R] ----
struct Spec {
  std::string kind;          // face | wrestler | stage | prop
  int n = 0;                 // wrestler slot / stage slot / prop id
  std::string from;          // "" = the kind's own default, else front | back | left | right | game
  double radius = 0;         // 0 = measured
};

inline bool ParseSpec(const std::string& text, Spec& out, std::string& err) {
  out = Spec();
  size_t pos = 0;
  std::vector<std::string> parts;
  while (pos <= text.size()) {
    const size_t c = text.find(',', pos);
    parts.push_back(text.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
    if (c == std::string::npos) break;
    pos = c + 1;
  }
  if (parts.empty() || parts[0].empty()) { err = "empty subject"; return false; }
  const size_t colon = parts[0].find(':');
  if (colon == std::string::npos) { err = "subject must be KIND:N (face:0 | wrestler:1 | stage:114 | prop:2004)"; return false; }
  out.kind = parts[0].substr(0, colon);
  if (out.kind != "face" && out.kind != "wrestler" && out.kind != "stage" && out.kind != "prop") { err = "unknown subject kind '" + out.kind + "' (face | wrestler | stage | prop)"; return false; }
  const std::string num = parts[0].substr(colon + 1);
  char* end = nullptr;
  const long v = std::strtol(num.c_str(), &end, 10);
  if (num.empty() || !end || *end || v < 0 || v > 1000000) { err = "bad number in subject '" + parts[0] + "'"; return false; }
  out.n = int(v);
  for (size_t i = 1; i < parts.size(); ++i) {
    const std::string& p = parts[i];
    if (p.rfind("from=", 0) == 0) {
      out.from = p.substr(5);
      if (out.from != "front" && out.from != "back" && out.from != "left" && out.from != "right" && out.from != "game") { err = "bad from=" + out.from + " (front | back | left | right | game)"; return false; }
    } else if (p.rfind("radius=", 0) == 0) {
      out.radius = std::strtod(p.c_str() + 7, nullptr);
      if (!(out.radius > 0)) { err = "bad radius"; return false; }
    } else { err = "unknown subject option '" + p + "'"; return false; }
  }
  return true;
}

// ---- 4x4 helpers (row vectors, row-major) ----
inline V3 XformPoint(const float* m, const V3& p) {
  V3 r;
  for (size_t j = 0; j < 3; ++j) r[j] = p[0] * double(m[j]) + p[1] * double(m[4 + j]) + p[2] * double(m[8 + j]) + double(m[12 + j]);
  return r;
}

// The world point of a skeleton node: the bind translation (the node's point in model space) times the palette row. The frame's palette already carries the body scale s
// (rf_wrestlers ScalePaletteRows); a caller with an UNSCALED palette passes s (p * diag(s,s,s,1) * M: the vertex times s first, body_scale.h). The head, the eyelids: by NAME, no fixed index.
inline V3 JointWorld(const assets::Matrix& bind_world, const float* palette16, double s = 1.0) {
  const V3 bp = {double(bind_world[12]) * s, double(bind_world[13]) * s, double(bind_world[14]) * s};
  return XformPoint(palette16, bp);
}
inline int FindBone(const std::vector<assets::Bone>& bones, const char* name) {
  for (size_t i = 0; i < bones.size(); ++i)
    if (bones[i].name == name) return int(i);
  return -1;
}

// ---- where the subject is and which way it faces ----
struct Subject {
  bool ok = false;
  std::string why;            // when !ok
  V3 target{}, normal{};      // the point the picture centres on; the unit direction the camera sits on (from the target toward the eye)
  double radius = 0;
  V3 forward{};               // a face / wrestler: the raw forward direction (the camera's `normal` may be this kept level)
  std::string source;         // how the facing was found: "eyelids" | "fallback: ..." | "from=..." | "area" ...
  bool piece = false;         // a stage subject: framed as a triangle set (PieceCamDir), `pieces` below
  wgs::Piece pieces;
};

inline V3 FlattenY(const V3& d) {
  const V3 h = {d[0], 0, d[2]};
  return wgs::Len(h) > 1e-6 ? wgs::Norm(h) : d;
}

// Rotate a horizontal facing: the camera sits on the subject's own side. +right = cross(up, forward) (the camera convention of MakeVP: x = up x z).
inline V3 SideOf(const V3& forward, const std::string& from, const V3& game_dir) {
  const V3 up = {0, 1, 0};
  if (from == "back") return {-forward[0], -forward[1], -forward[2]};
  if (from == "right") return wgs::Norm(wgs::Cross(up, forward));
  if (from == "left") { const V3 r = wgs::Norm(wgs::Cross(up, forward)); return {-r[0], -r[1], -r[2]}; }
  if (from == "game") return game_dir;
  return forward;
}

// The face's forward direction (note claim 4): (eyelid midpoint - head joint) made perpendicular to the eye axis (r - l). ok = false when there are no two eyelid points or they coincide.
inline bool FaceForward(const V3& head, const V3& l_lid, const V3& r_lid, V3& forward, V3& eyes) {
  eyes = {0.5 * (l_lid[0] + r_lid[0]), 0.5 * (l_lid[1] + r_lid[1]), 0.5 * (l_lid[2] + r_lid[2])};
  const V3 axis = wgs::Norm(wgs::Sub(r_lid, l_lid));
  V3 d = wgs::Sub(eyes, head);
  if (wgs::Len(axis) < 0.5) return false;
  const double along = wgs::Dot(d, axis);
  d = {d[0] - axis[0] * along, d[1] - axis[1] * along, d[2] - axis[2] * along};
  if (wgs::Len(d) < 1e-6) return false;
  forward = wgs::Norm(d);
  return true;
}

// face:N. eyelids given (have_lids) -> target = their midpoint, forward from FaceForward, radius = 1.5 x |eyes - head| (>= 12 units) unless given. No eyelids: target = the head joint, the game's direction, labelled.
inline Subject FaceSubject(const V3& head, bool have_lids, const V3& l_lid, const V3& r_lid, const V3& game_dir, const Spec& sp) {
  Subject s;
  V3 fwd{}, eyes{};
  const bool lids = have_lids && FaceForward(head, l_lid, r_lid, fwd, eyes);
  s.target = lids ? eyes : head;
  s.radius = sp.radius > 0 ? sp.radius : std::max(12.0, lids ? 1.5 * wgs::Len(wgs::Sub(eyes, head)) : 20.0);
  if (lids) {
    // "in front of the face" = at the face's height: the eyelids sit above the head joint, so the raw forward points a little UP; an upright head (|forward.y| < 0.85) is framed level
    // (flattened to the horizontal), a head that is not upright (lying down: the face looks up) keeps the raw direction, labelled.
    const bool upright = std::fabs(fwd[1]) < 0.85;
    const V3 base = upright ? FlattenY(fwd) : fwd;
    s.forward = fwd;
    s.normal = sp.from.empty() ? base : SideOf(base, sp.from, game_dir);
    s.source = std::string("eyelids") + (upright ? " (level)" : " (tilted: head not upright)") + (sp.from.empty() || sp.from == "front" ? "" : ", from=" + sp.from);
  } else {
    s.normal = game_dir;
    s.source = "fallback: no eyelid nodes, the direction the game's camera came from";
  }
  s.ok = true;
  return s;
}

// wrestler:N. box = the object's sampled skinned vertices (the MeasureCasters rule: the sphere is the box's half diagonal). forward: the face's (lids) or the game's direction.
inline Subject WrestlerSubject(const V3& lo, const V3& hi, bool have_forward, const V3& forward, const V3& game_dir, const Spec& sp) {
  Subject s;
  s.target = {0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2])};
  const V3 h = {0.5 * (hi[0] - lo[0]), 0.5 * (hi[1] - lo[1]), 0.5 * (hi[2] - lo[2])};
  s.radius = sp.radius > 0 ? sp.radius : wgs::Len(h);
  if (have_forward) {
    s.forward = forward;
    s.normal = SideOf(FlattenY(forward), sp.from, game_dir);
    s.source = sp.from.empty() || sp.from == "front" ? "eyelids" : "eyelids, from=" + sp.from;
  } else {
    s.normal = FlattenY(game_dir);
    s.source = "fallback: no face forward, the direction the game's camera came from";
  }
  s.ok = true;
  return s;
}

// prop:ID. A point and a radius (the loaded model's box where the caller has it, else a told / default radius that the caller labels).
inline Subject PropSubject(const V3& pos, double radius, const V3& game_dir, const Spec& sp) {
  Subject s;
  s.target = pos;
  s.radius = sp.radius > 0 ? sp.radius : radius;
  s.normal = FlattenY(game_dir);
  s.source = "the direction the game's camera came from";
  s.ok = true;
  return s;
}

// The camera that frames a sphere (target, radius) from direction n (unit, target -> eye), in VIEWER space (y up). 30 degree vertical FOV at 16:9, as PieceCamDir; 10 % margin.
inline Cam SphereCam(const V3& target, double radius, V3 n) {
  n = wgs::Len(n) > 1e-9 ? wgs::Norm(n) : V3{0, 0, -1};
  Cam c;
  c.fov = 30;
  const double half = 0.5 * c.fov * 3.14159265358979 / 180.0;
  c.dist = std::max(1.0, 1.1 * radius / std::sin(half));
  c.target = target;
  for (size_t j = 0; j < 3; ++j) c.eye[j] = target[j] + n[j] * c.dist;
  c.near_z = std::max(0.1, 0.25 * (c.dist - radius));
  c.far_z = c.dist + radius * 3.0 + 10.0;
  wgs::MakeVP(c, {0, 1, 0});
  return c;
}

inline Cam CameraOf(const Subject& s) {
  if (s.piece) return wgs::PieceCamDir(s.pieces, s.normal);
  return SphereCam(s.target, s.radius, s.normal);
}

// stage:S. `tris` = the slot's triangles (all copies: the slot, its `:r` and `:y` twins) in VIEWER space, wound as DRAWN (a mirrored copy's corners swapped by the caller), `eye` = the record's camera eye in
// viewer space. The normal is the facing that saw the most area from that eye: triangles facing the eye are binned by their rounded unit normal (0.05), the bin with the most area wins (a ring of panels
// whose normals cancel in a sum still gives one panel group). from= front = that facing, back = its opposite, left / right = the sides of it, game = from the eye. No triangle faces the eye: the
// largest bin of all triangles, labelled. radius= is ignored (the fit is the triangle set).
inline Subject StageSubject(const std::vector<wgs::Tri>& tris, const V3& eye, const Spec& sp) {
  Subject s;
  if (tris.empty()) { s.why = "the stage slot has no triangles in the record's arena model"; return s; }
  struct Bin { double area = 0; V3 sum{}; };
  std::map<std::array<long long, 3>, Bin> seen, all;
  wgs::Piece p;
  V3 csum{};
  double area_all = 0;
  for (const wgs::Tri& t : tris) {
    const V3 nn = wgs::Cross(wgs::Sub(t.p[1], t.p[0]), wgs::Sub(t.p[2], t.p[0]));
    const double a = 0.5 * wgs::Len(nn);
    for (int j = 0; j < 3; ++j) p.pts.push_back(t.p[j]);
    if (a <= 1e-12) continue;
    const V3 mid = {(t.p[0][0] + t.p[1][0] + t.p[2][0]) / 3.0, (t.p[0][1] + t.p[1][1] + t.p[2][1]) / 3.0, (t.p[0][2] + t.p[1][2] + t.p[2][2]) / 3.0};
    for (size_t j = 0; j < 3; ++j) csum[j] += a * mid[j];
    area_all += a;
    const V3 u = wgs::Norm(nn);
    const std::array<long long, 3> key = {std::llround(u[0] / 0.05), std::llround(u[1] / 0.05), std::llround(u[2] / 0.05)};
    Bin& b = all[key];
    b.area += a;
    for (size_t j = 0; j < 3; ++j) b.sum[j] += nn[j] * 0.5;
    if (wgs::Dot(nn, wgs::Sub(eye, mid)) > 0) {
      Bin& q = seen[key];
      q.area += a;
      for (size_t j = 0; j < 3; ++j) q.sum[j] += nn[j] * 0.5;
    }
  }
  if (area_all <= 0) { s.why = "the stage slot's triangles have no area"; return s; }
  p.centroid = {csum[0] / area_all, csum[1] / area_all, csum[2] / area_all};
  p.tris = tris.size();
  std::map<std::array<long long, 3>, Bin>& use = seen.empty() ? all : seen;
  const Bin* best = nullptr;
  for (const auto& kv : use)
    if (!best || kv.second.area > best->area) best = &kv.second;
  V3 facing = wgs::Norm(best->sum);
  s.source = seen.empty() ? "fallback: no triangle faces the game's camera, the largest facing" : "the facing that saw the most area";
  // The picture frames the panels of THAT facing (the strip the game's camera saw), not the whole slot: a slot that wraps the hall (panels on opposite walls) would put the eye outside the far wall.
  const V3 group_dir = wgs::Norm(best->sum);
  wgs::Piece q;
  V3 qsum{};
  double qarea = 0;
  for (size_t i = 0; i + 2 < p.pts.size(); i += 3) {
    const V3 nn = wgs::Cross(wgs::Sub(p.pts[i + 1], p.pts[i]), wgs::Sub(p.pts[i + 2], p.pts[i]));
    const double a = 0.5 * wgs::Len(nn);
    if (a <= 1e-12 || wgs::Dot(wgs::Norm(nn), group_dir) < 0.9) continue;
    for (size_t k = 0; k < 3; ++k) {
      q.pts.push_back(p.pts[i + k]);
      for (size_t j = 0; j < 3; ++j) qsum[j] += a * p.pts[i + k][j] / 3.0;
    }
    qarea += a;
    ++q.tris;
  }
  if (qarea > 0) { q.centroid = {qsum[0] / qarea, qsum[1] / qarea, qsum[2] / qarea}; p = std::move(q); }
  const V3 game_dir = wgs::Norm(wgs::Sub(eye, p.centroid));
  if (sp.from == "back") facing = {-facing[0], -facing[1], -facing[2]}, s.source += ", from=back";
  else if (sp.from == "right" || sp.from == "left") facing = SideOf(FlattenY(facing), sp.from, game_dir), s.source += ", from=" + sp.from;
  else if (sp.from == "game") facing = game_dir, s.source = "from=game: the direction the game's camera came from";
  s.normal = facing;
  s.target = p.centroid;
  s.piece = true;
  s.pieces = std::move(p);
  double rr = 0;
  for (const V3& q : s.pieces.pts) rr = std::max(rr, wgs::Len(wgs::Sub(q, s.target)));
  s.radius = rr;
  s.ok = true;
  return s;
}

// ---- the game's space -> the rig. The camera is built in viewer space; the rig's --camera / a Live record's camera is GAME space. ----
struct GameCam { float vp[16]; float eye[3]; };
inline GameCam ToGameCam(const Cam& c) {
  GameCam g;
  wgs::ToGame(c, g.vp, g.eye);
  return g;
}
// The PLANTED TWIN of the unit (--plant no_togame): the viewer camera handed in as it is. BuildFrame then mirrors it (row 1 and the eye's y) and the picture is the mirror image.
inline GameCam NoToGame(const Cam& c) {
  GameCam g;
  for (int i = 0; i < 16; ++i) g.vp[i] = c.vp[i];
  g.eye[0] = float(c.eye[0]), g.eye[1] = float(c.eye[1]), g.eye[2] = float(c.eye[2]);
  return g;
}
// What BuildFrame does to a Live record's camera (record_frame.cpp): view row 1 and the eye's y negate -> the viewer-space view-projection the frame draws with.
inline void BuildFrameCamera(const GameCam& g, float vp_viewer[16], float eye_viewer[3]) {
  for (int i = 0; i < 16; ++i) vp_viewer[i] = (i >= 4 && i < 8) ? -g.vp[i] : g.vp[i];
  eye_viewer[0] = g.eye[0], eye_viewer[1] = -g.eye[1], eye_viewer[2] = g.eye[2];
}

// ---- pixel <-> world (the rig's picture: row 0 = the top = world up; clip y + = up) ----
struct Px { bool ok = false; double x = 0, y = 0, depth = 0; };
inline Px Project(const float* vp, const V3& p, int w, int h) {
  Px r;
  double c[4];
  for (size_t j = 0; j < 4; ++j) c[j] = p[0] * double(vp[j]) + p[1] * double(vp[4 + j]) + p[2] * double(vp[8 + j]) + double(vp[12 + j]);
  if (c[3] <= 1e-9) return r;
  r.ok = true;
  r.x = (c[0] / c[3] * 0.5 + 0.5) * w;
  r.y = (0.5 - c[1] / c[3] * 0.5) * h;
  r.depth = c[2] / c[3];
  return r;
}

inline bool Invert4(const double* m, double* inv) {   // general 4x4, row-major
  double a[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) a[i][j] = m[i * 4 + j], a[i][4 + j] = (i == j) ? 1.0 : 0.0;
  for (int c = 0; c < 4; ++c) {
    int piv = c;
    for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
    if (std::fabs(a[piv][c]) < 1e-30) return false;
    if (piv != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[piv][j]);
    const double d = a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      if (f == 0) continue;
      for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) inv[i * 4 + j] = a[i][4 + j];
  return true;
}

struct Ray { bool ok = false; V3 origin{}, dir{}; };
// The ray through the CENTRE of pixel (px, py): the two points of the view-projection's inverse at depth 0 and 1.
inline Ray PixelRay(const float* vp, int w, int h, double px, double py) {
  Ray r;
  double m[16], inv[16];
  for (int i = 0; i < 16; ++i) m[i] = vp[i];
  if (!Invert4(m, inv)) return r;
  const double nx = 2.0 * px / w - 1.0, ny = 1.0 - 2.0 * py / h;
  V3 pts[2];
  for (int k = 0; k < 2; ++k) {
    const double c[4] = {nx, ny, double(k), 1.0};
    double q[4];
    for (int j = 0; j < 4; ++j) q[j] = c[0] * inv[0 * 4 + j] + c[1] * inv[1 * 4 + j] + c[2] * inv[2 * 4 + j] + c[3] * inv[3 * 4 + j];
    if (std::fabs(q[3]) < 1e-30) return r;
    pts[k] = {q[0] / q[3], q[1] / q[3], q[2] / q[3]};
  }
  r.origin = pts[0];
  r.dir = wgs::Norm(wgs::Sub(pts[1], pts[0]));
  r.ok = wgs::Len(r.dir) > 0.5;
  return r;
}

// Moller-Trumbore, two-sided. t = the distance along the unit dir. front = the triangle's drawn front (cross(b - a, c - a), the convention of ExpectPx) faces the ray's origin.
inline bool RayTri(const Ray& r, const V3& a, const V3& b, const V3& c, double& t, bool& front) {
  const V3 e1 = wgs::Sub(b, a), e2 = wgs::Sub(c, a);
  const V3 pv = wgs::Cross(r.dir, e2);
  const double det = wgs::Dot(e1, pv);
  if (std::fabs(det) < 1e-18) return false;
  const double inv = 1.0 / det;
  const V3 tv = wgs::Sub(r.origin, a);
  const double u = wgs::Dot(tv, pv) * inv;
  if (u < 0 || u > 1) return false;
  const V3 qv = wgs::Cross(tv, e1);
  const double v = wgs::Dot(r.dir, qv) * inv;
  if (v < 0 || u + v > 1) return false;
  t = wgs::Dot(e2, qv) * inv;
  if (t < 0) return false;
  front = wgs::Dot(wgs::Cross(e1, e2), r.dir) < 0;
  return true;
}

// ---- the pick: one mesh with its world-space triangles and the draw facts that decide what it hides ----
struct PickMesh {
  std::string object;      // "arena:slot:114"
  std::string part;        // Mesh::part ("slot 114", "body")
  std::string shader;      // Material::shader
  std::string texture;     // Material::diffuse_name, else the diffuse texture's name
  int ordinal = 0;         // the mesh's number inside its object (arena slot: its n-th mesh)
  int src_record = -1;
  bool blend = false, tested = false, cull_back = false;
  std::vector<V3> tris;    // 3 corners a triangle, world (viewer) space, wound as drawn
};
struct Hit {
  size_t mesh = 0;
  double dist = 0;         // from the ray origin (the caller passes the eye as the origin of the listing's distance)
  bool front = true;
  int tri_hits = 1;        // how many of this mesh's triangles the ray crosses
  V3 pos{};
};

// Every mesh the ray crosses, front to back (nearest triangle of each mesh; `tri_hits` counts the rest). nearest_only is the PLANTED TWIN (--pick-plant nearest_only): the rig used to answer with the first hit
// only, which hides a blended or tested surface lying over another (BAN2's 806-over-band case).
inline std::vector<Hit> PickAll(const Ray& r, const std::vector<PickMesh>& meshes, const V3& eye, bool nearest_only = false) {
  std::vector<Hit> hits;
  for (size_t mi = 0; mi < meshes.size(); ++mi) {
    const PickMesh& m = meshes[mi];
    Hit best;
    int n = 0;
    for (size_t i = 0; i + 2 < m.tris.size(); i += 3) {
      double t = 0;
      bool front = true;
      if (!RayTri(r, m.tris[i], m.tris[i + 1], m.tris[i + 2], t, front)) continue;
      ++n;
      if (n == 1 || t < best.dist) best.mesh = mi, best.dist = t, best.front = front;
    }
    if (!n) continue;
    best.tri_hits = n;
    for (size_t j = 0; j < 3; ++j) best.pos[j] = r.origin[j] + r.dir[j] * best.dist;
    best.dist = wgs::Len(wgs::Sub(best.pos, eye));
    hits.push_back(best);
  }
  std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.dist < b.dist; });
  if (nearest_only && hits.size() > 1) hits.resize(1);
  return hits;
}

inline std::string HitLine(const Hit& h, const PickMesh& m, size_t rank) {
  char b[640];
  std::snprintf(b, sizeof b, "pick hit %zu: %s mesh %d (src_record %d) part '%s' %s %s dist %.1f %s%s%s%s tri_hits %d at %.1f,%.1f,%.1f", rank, m.object.c_str(), m.ordinal, m.src_record, m.part.c_str(), m.shader.c_str(),
                m.texture.empty() ? "-" : m.texture.c_str(), h.dist, h.front ? "front" : "back", m.blend ? " blended" : "", m.tested ? " alpha-tested" : "", m.cull_back ? " cull-back" : "", h.tri_hits, h.pos[0], h.pos[1], h.pos[2]);
  return b;
}

}  // namespace subj
