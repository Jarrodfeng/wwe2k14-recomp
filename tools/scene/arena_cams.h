// WGS round 2 (docs\agents\arena_sheet.md): the cameras of the offline arena sheet, from the arena model alone (viewer space, y up; units as the files). Header only, no GPU, no game.
//   * the screen CHECK pass: every screen key 800-809 split into connected pieces (SplitPieces), one front-on camera per kept piece (PieceCam);
//   * the BEAUTY view per look, for Jarrod's eyes only (BeautyCam): from the far side of the ring toward the stage. No number is taken from it.
// Row vectors (p' = p * M), z forward, depth 0..1, the view-projection in one matrix (kCamViewIsViewProj), as frame_rig.cpp SynthCamera makes it. ToGame turns a viewer camera into the
// game-space one a Live record / frame_rig --camera carries (row 1 of the matrix and the eye's y negated: BuildFrame applies the inverse).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/asset_model.h"

namespace wgs {

using V3 = std::array<double, 3>;
struct Tri {
  V3 p[3];
  float uv[3][2] = {};     // uv0 (the live picture's coordinates)
  uint32_t col[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};   // the vertex colour as stored (alpha = bits 24-31, as rf_c_core.cpp's movie-surface rule reads it)
};

inline V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 Cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
inline double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(const V3& a) { const double l = Len(a); return l > 1e-12 ? V3{a[0] / l, a[1] / l, a[2] / l} : V3{0, 0, 0}; }

// The key (800-809) of an arena mesh whose part is exactly "slot K", else -1.
inline int ScreenKeyOf(const assets::Mesh& me) {
  int k = 0;
  if (std::sscanf(me.part.c_str(), "slot %d", &k) != 1 || me.part != "slot " + std::to_string(k) || k < 800 || k > 809) return -1;
  return k;
}

// Every screen key's triangles, placements applied (each transform of the mesh's placement: a mirrored twin is its own copy).
inline std::map<int, std::vector<Tri>> ScreenTris(const assets::Model& model) {
  std::map<int, std::vector<Tri>> out;
  for (const assets::Mesh& me : model.meshes) {
    const int k = ScreenKeyOf(me);
    if (k < 0) continue;
    std::vector<assets::Matrix> xf;
    if (me.placement >= 0 && size_t(me.placement) < model.placements.size()) xf = model.placements[size_t(me.placement)].transforms;
    if (xf.empty()) xf.push_back(assets::Identity());
    for (const assets::Matrix& M : xf)
      for (size_t i = 0; i + 2 < me.indices.size(); i += 3) {
        Tri t;
        bool ok = true;
        for (int j = 0; j < 3; ++j) {
          const uint32_t ix = me.indices[i + j];
          if (ix >= me.vertices.size()) { ok = false; break; }
          const float* v = me.vertices[ix].pos;
          for (int c = 0; c < 3; ++c) t.p[j][c] = double(v[0]) * M[0 * 4 + c] + double(v[1]) * M[1 * 4 + c] + double(v[2]) * M[2 * 4 + c] + M[12 + c];
          t.uv[j][0] = me.vertices[ix].uv0[0], t.uv[j][1] = me.vertices[ix].uv0[1], t.col[j] = me.vertices[ix].color;
        }
        if (ok) out[k].push_back(t);
      }
  }
  return out;
}

struct Piece {
  int key = 0, index = 0;
  double area = 0;
  V3 centroid{}, lo{}, hi{}, normal{};   // area-weighted centroid; box; area-weighted geometric normal (unit; the cross product (b - a) x (c - a) of the stored winding)
  bool weak_normal = false;              // |sum of the triangle normals| < 0.3 of the area (a closed or folded piece): the normal is the horizontal direction toward the ring centre instead
  size_t tris = 0;
  std::vector<V3> pts;                   // the piece's triangle corners
  std::vector<std::array<float, 2>> uvs; // their uv0 (parallel to pts)
  std::vector<uint32_t> cols;            // their vertex colours (parallel to pts)
  bool blended = false;                  // a corner with vertex alpha < 255: drawn SrcAlpha / InvSrcAlpha (rf_c_core.cpp blend_inline)
  std::vector<size_t> tri_id;            // WGS6: per triangle (3 consecutive pts), its index in the KEY's triangle list = the key's draw (index) order, for the occlusion model of ExpectPx
};

// The connected pieces of one key (the SPATIAL rule: corners welded by position rounded to 1 unit, triangles sharing a welded corner are one piece), largest area first; kept = the largest,
// plus any further piece with at least `min_share` of its area, `max_n` at most. `all` (optional) receives every piece, kept or not.
inline std::vector<Piece> SplitPieces(int key, const std::vector<Tri>& tris, double min_share = 0.25, size_t max_n = 3, std::vector<Piece>* all = nullptr) {
  std::map<std::array<long long, 3>, int> id;
  std::vector<int> parent;
  const auto find = [&](int x) { while (parent[size_t(x)] != x) x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])]; return x; };
  std::vector<std::array<int, 3>> tv(tris.size());
  for (size_t t = 0; t < tris.size(); ++t)
    for (int j = 0; j < 3; ++j) {
      const std::array<long long, 3> q = {std::llround(tris[t].p[j][0]), std::llround(tris[t].p[j][1]), std::llround(tris[t].p[j][2])};
      auto it = id.find(q);
      if (it == id.end()) it = id.emplace(q, int(parent.size())).first, parent.push_back(int(parent.size()));
      tv[t][size_t(j)] = it->second;
    }
  for (const auto& v : tv) {
    const int a = find(v[0]), b = find(v[1]), c = find(v[2]);
    parent[size_t(b)] = a;
    parent[size_t(find(c))] = a;
  }
  std::map<int, Piece> by;
  std::map<int, V3> nsum, csum;
  for (size_t t = 0; t < tris.size(); ++t) {
    const int r = find(tv[t][0]);
    Piece& p = by[r];
    const V3 n = Cross(Sub(tris[t].p[1], tris[t].p[0]), Sub(tris[t].p[2], tris[t].p[0]));
    const double a = 0.5 * Len(n);
    if (p.tris == 0) p.lo = p.hi = tris[t].p[0];
    ++p.tris;
    p.tri_id.push_back(t);
    p.area += a;
    V3& ns = nsum[r];
    V3& cs = csum[r];
    for (int j = 0; j < 3; ++j) {
      ns[size_t(j)] += 0.5 * n[size_t(j)];
      cs[size_t(j)] += a * (tris[t].p[0][size_t(j)] + tris[t].p[1][size_t(j)] + tris[t].p[2][size_t(j)]) / 3.0;
    }
    for (int j = 0; j < 3; ++j) {
      p.pts.push_back(tris[t].p[j]);
      p.uvs.push_back({tris[t].uv[j][0], tris[t].uv[j][1]});
      p.cols.push_back(tris[t].col[j]);
      if (((tris[t].col[j] >> 24) & 0xFFu) < 0xFFu) p.blended = true;
      for (int c = 0; c < 3; ++c) p.lo[size_t(c)] = std::min(p.lo[size_t(c)], tris[t].p[j][size_t(c)]), p.hi[size_t(c)] = std::max(p.hi[size_t(c)], tris[t].p[j][size_t(c)]);
    }
  }
  std::vector<Piece> v;
  for (auto& kv : by) {
    Piece& p = kv.second;
    p.key = key;
    const V3& cs = csum[kv.first];
    if (p.area > 0) p.centroid = {cs[0] / p.area, cs[1] / p.area, cs[2] / p.area};
    else p.centroid = {0.5 * (p.lo[0] + p.hi[0]), 0.5 * (p.lo[1] + p.hi[1]), 0.5 * (p.lo[2] + p.hi[2])};
    const V3& ns = nsum[kv.first];
    if (Len(ns) < 0.3 * p.area) {
      p.weak_normal = true;
      V3 h = {-p.centroid[0], 0, -p.centroid[2]};
      p.normal = Len(h) > 1e-6 ? Norm(h) : V3{0, 0, -1};
    } else {
      p.normal = Norm(ns);
    }
    v.push_back(std::move(p));
  }
  std::stable_sort(v.begin(), v.end(), [](const Piece& a, const Piece& b) {   // largest first; equal areas (mirror twins: a54 808 26 / 27) by centroid, so their indices never swap
    if (a.area != b.area) return a.area > b.area;
    return a.centroid < b.centroid;
  });
  for (size_t i = 0; i < v.size(); ++i) v[i].index = int(i);
  if (all) *all = v;
  std::vector<Piece> kept;
  for (size_t i = 0; i < v.size() && kept.size() < max_n; ++i)
    if (i == 0 || v[i].area >= min_share * v[0].area) kept.push_back(v[i]);
  return kept;
}

struct Cam {
  V3 eye{}, target{};
  double fov = 30, near_z = 1, far_z = 1000, dist = 0;
  float vp[16] = {};
};

// The view-projection (vertical FOV, 16:9), as SynthCamera: row vectors, z forward, depth 0..1.
inline void MakeVP(Cam& c, V3 up) {
  V3 z = Norm(Sub(c.target, c.eye));
  if (std::fabs(Dot(z, Norm(up))) > 0.98) up = std::fabs(z[1]) > 0.9 ? V3{0, 0, 1} : V3{0, 1, 0};
  const V3 x = Norm(Cross(up, z)), y = Cross(z, x);
  const V3& e = c.eye;
  const double V[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -Dot(x, e), -Dot(y, e), -Dot(z, e), 1};
  const double ys = 1.0 / std::tan(0.5 * c.fov * 3.14159265358979 / 180.0), xs = ys * 9.0 / 16.0, zn = c.near_z, zf = c.far_z;
  const double P[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += V[i * 4 + k] * P[k * 4 + j];
      c.vp[i * 4 + j] = float(s);
    }
}

// The front-on camera of one piece: eye on the normal through the piece (back = the other side), far enough to fit the piece with a 10 % margin in a 30 degree vertical FOV at 16:9.
inline Cam PieceCamDir(const Piece& p, const V3& n);
inline Cam PieceCam(const Piece& p, bool back) { return PieceCamDir(p, back ? V3{-p.normal[0], -p.normal[1], -p.normal[2]} : p.normal); }
// The same framing from any direction n (unit): eye on n through the piece. A weak-normal (closed / folded) piece gets four horizontal ones (+x, -x, +z, -z).
inline Cam PieceCamDir(const Piece& p, const V3& n) {
  const V3 up = std::fabs(n[1]) > 0.9 ? V3{0, 0, 1} : V3{0, 1, 0};   // a flat piece (the floor copies) is framed with z up
  const V3 zf = {-n[0], -n[1], -n[2]};
  const V3 x = Norm(Cross(up, zf)), y = Cross(zf, x);
  double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30, d0 = 1e30, d1 = -1e30;
  for (const V3& q : p.pts) {
    const V3 r = Sub(q, p.centroid);
    x0 = std::min(x0, Dot(r, x)), x1 = std::max(x1, Dot(r, x)), y0 = std::min(y0, Dot(r, y)), y1 = std::max(y1, Dot(r, y)), d0 = std::min(d0, Dot(r, n)), d1 = std::max(d1, Dot(r, n));
  }
  if (p.pts.empty()) x0 = x1 = y0 = y1 = d0 = d1 = 0;
  Cam c;
  c.fov = 30;
  const double tv = std::tan(0.5 * c.fov * 3.14159265358979 / 180.0);
  const double hx = 0.5 * (x1 - x0), hy = 0.5 * (y1 - y0);
  c.dist = std::max(1.0, d1 + 1.1 * std::max(hy / tv, hx / (tv * 16.0 / 9.0)));
  for (int j = 0; j < 3; ++j) {
    c.target[size_t(j)] = p.centroid[size_t(j)] + x[size_t(j)] * 0.5 * (x0 + x1) + y[size_t(j)] * 0.5 * (y0 + y1);
    c.eye[size_t(j)] = c.target[size_t(j)] + n[size_t(j)] * c.dist;
  }
  c.near_z = std::max(0.1, 0.25 * (c.dist - d1));
  c.far_z = c.dist - d0 + 0.5 * c.dist + 10.0;
  MakeVP(c, up);
  return c;
}

// The beauty view of one look (for the eyes; no number is taken from it). `stage` = the stage point (the largest piece of the look's big key, else the stage rect's centre at the canvas top),
// `stage_lo` / `stage_hi` its box. facing = ring centre -> stage (horizontal); eye on the far side, `eye_dist` (1300) from the ring centre clamped to a third of the arena's half-extent that way,
// `eye_up` (550) above the canvas top; aim midway between the ring centre (at the canvas top) and the stage point raised to 40 % of its height; FOV = what fits the ring's flat top and the stage box,
// clamped to 40-70 degrees.
inline Cam BeautyCam(const assets::Model& model, const V3& stage, const V3& stage_lo, const V3& stage_hi, double canvas_top, double eye_dist = 1300, double eye_up = 550) {
  V3 f = {stage[0], 0, stage[2]};
  f = Len(f) > 1e-6 ? Norm(f) : V3{0, 0, 1};
  double ext = 0;   // the arena's half-extent on the far side
  for (const assets::Mesh& me : model.meshes)
    for (const assets::Vertex& v : me.vertices) ext = std::max(ext, -(double(v.pos[0]) * f[0] + double(v.pos[2]) * f[2]));
  const double d = ext > 0 ? std::min(eye_dist, ext / 3.0) : eye_dist;
  Cam c;
  c.dist = d;
  c.eye = {-f[0] * d, canvas_top + eye_up, -f[2] * d};
  const double sy = stage_lo[1] + 0.4 * (stage_hi[1] - stage_lo[1]);
  c.target = {0.5 * stage[0], 0.5 * (canvas_top + sy), 0.5 * stage[2]};
  const V3 z = Norm(Sub(c.target, c.eye)), x = Norm(Cross(V3{0, 1, 0}, z)), y = Cross(z, x);
  double need = 0;
  std::vector<V3> pts;
  for (double sx : {-310.0, 310.0})
    for (double sz : {-310.0, 310.0}) pts.push_back({sx, canvas_top, sz});
  for (int i = 0; i < 8; ++i) pts.push_back({(i & 1) ? stage_hi[0] : stage_lo[0], (i & 2) ? stage_hi[1] : stage_lo[1], (i & 4) ? stage_hi[2] : stage_lo[2]});
  for (const V3& q : pts) {
    const V3 r = Sub(q, c.eye);
    const double zz = Dot(r, z);
    if (zz <= 1) continue;
    const double ay = std::atan(std::fabs(Dot(r, y)) / zz), ax = std::atan(std::tan(std::atan(std::fabs(Dot(r, x)) / zz)) * 9.0 / 16.0);
    need = std::max(need, std::max(ay, ax));
  }
  c.fov = std::min(70.0, std::max(40.0, 2.0 * need * 180.0 / 3.14159265358979 * 1.05));
  c.near_z = 10, c.far_z = std::max(20000.0, 4.0 * ext + 2000.0);
  MakeVP(c, {0, 1, 0});
  return c;
}

// WGS6: the key's triangles in the game's DRAW order. A screen key is one mesh = one draw: its triangles go down in index order (ScreenTris keeps that order: mesh, then each placement
// transform, then the index list); Piece::tri_id is each triangle's place in it. `all` = every piece of the key (SplitPieces' `all`). Built once per key, not per view.
struct TriRef {
  size_t id = 0;               // the triangle's index in the key's list (the draw order)
  const Piece* piece = nullptr;
  size_t t = 0;                // the triangle's number inside piece->pts (corners 3t .. 3t+2)
};
inline std::vector<TriRef> KeyDrawOrder(const std::vector<Piece>& all) {
  std::vector<TriRef> v;
  for (const Piece& q : all)
    for (size_t t = 0; t < q.tri_id.size(); ++t) v.push_back({q.tri_id[t], &q, t});
  std::sort(v.begin(), v.end(), [](const TriRef& a, const TriRef& b) { return a.id < b.id; });
  return v;
}

// WGS fix round + WGS6: the pixels a piece SHOULD cover from camera c at w x h, and what the camera should see there.
// The key is drawn as the game draws it (mesh = one draw, triangles in index order, camera-facing ones only: movie keys are back-culled, depth test + write on, the game's alpha test GEQUAL 1 / 255
// on the interpolated vertex alpha, SrcAlpha / InvSrcAlpha blend; the picture lands only where uv0 is inside [0, 1]: outside it the sampler gives no picture and the fragment is not drawn).
// Every triangle of the key (`key_order`, KeyDrawOrder of all its pieces; null = the piece alone) is clipped to the camera's near / far planes and rasterised at pixel centres (top-left rule: a shared
// edge counts once) with a depth buffer, in draw order. A fragment of another piece that is nearer and was drawn EARLIER hides the judged piece there (the depth test rejects it); a nearer piece drawn
// LATER blends over it. So the expected footprint is the pixels where a fragment of THIS piece is actually drawn, and the planes carry the composite of everything drawn there:
//   alpha = 1 - prod(1 - a_i), colour_c = sum_i a_i v_ic prod_{j later}(1 - a_j) / alpha   (a_i, v_ic = the layer's vertex alpha and colour channel, perspective-correct)
// which is exactly alpha x colour x texel + (1 - alpha) x clear for a texel of one colour: the judge's model (arena_sheet.py flat_ratio / pattern_fit).
// WGS6: before this the key was NOT modelled ("nothing occludes in a screens-only render": false for a54 808 / 809, where closed glow volumes sit in front of each other, A54.md) and the vertex colour
// was the largest of its three colour bytes (a tinted colour, R 255 G 216 B 159, was untinted).
// Outputs (all optional): `footprint` one byte per pixel, row 0 = NDC y -1 (the driver checks the orientation against a drawn piece); `vcol` THREE bytes per pixel (R, G, B: the composite colour
// 0..255 the movie texel is multiplied by, per channel: bits 0-7 R, 8-15 G, 16-23 B of the stored word); `valpha` one byte per pixel (the composite alpha 0..255); `hidden` the pixels where this piece
// had a fragment (uv in range, alpha passed) that an earlier nearer piece covered (not in the footprint).
// [L] limits: one mesh per key is the draw order (a key with several meshes is taken in model order); fragments whose alpha the game discards write no depth (the game's rule; our renderer writes depth
// for them: SZW_contract.md site 18, the leftover difference the check then shows); the texel's own alpha is taken as 1 (the check's pictures have none).
// WGS6 round 2: the unit's planted twins (arena_cams_unit.cpp --plant): 1 = the key's draw order reversed, 2 = the depth test reversed (the farther fragment wins), 3 = the alpha test off (a fragment
// below 1 / 255 is drawn and writes depth). Test-only: nothing but the unit sets it.
inline int g_expect_plant = 0;
inline uint64_t ExpectPx(const Piece& p, const Cam& c, int w, int h, std::vector<uint8_t>* footprint = nullptr, std::vector<uint8_t>* vcol = nullptr, std::vector<uint8_t>* valpha = nullptr,
                         const std::vector<TriRef>* key_order = nullptr, uint64_t* hidden = nullptr) {
  const size_t npx = size_t(w) * size_t(h);
  std::vector<uint8_t> local;
  std::vector<uint8_t>& hit = footprint ? *footprint : local;
  hit.assign(npx, 0);
  std::vector<TriRef> own;
  if (!key_order) {   // the piece alone (no key context)
    for (size_t t = 0; t < p.tri_id.size(); ++t) own.push_back({t, &p, t});
    key_order = &own;
  }
  const bool depth_rev = g_expect_plant == 2;
  std::vector<TriRef> reversed;
  if (g_expect_plant == 1) reversed.assign(key_order->rbegin(), key_order->rend()), key_order = &reversed;
  std::vector<double> zb(npx, depth_rev ? -1e300 : 1e300), T(npx, 1.0), S(npx * 3, 0.0);   // depth; transmittance prod(1 - a); sum_i a_i v_i prod_{later}(1 - a_j) per channel (0..1)
  std::vector<uint8_t> mine(npx, 0);                                   // this piece had a fragment here (uv in range, alpha passed)
  struct Vx { double c[4]; double at[6]; };                            // clip position; u, v, alpha (0..255), r, g, b (0..255)
  for (const TriRef& ref : *key_order) {
    const Piece& q = *ref.piece;
    const bool is_mine = q.index == p.index && q.key == p.key;
    const size_t t = ref.t * 3;
    if (t + 2 >= q.pts.size()) continue;
    const V3 &a = q.pts[t], &b = q.pts[t + 1], &cc = q.pts[t + 2];
    const V3 nn = Cross(Sub(b, a), Sub(cc, a));
    const V3 mid = {(a[0] + b[0] + cc[0]) / 3.0, (a[1] + b[1] + cc[1]) / 3.0, (a[2] + b[2] + cc[2]) / 3.0};
    if (Dot(nn, Sub(c.eye, mid)) <= 0) continue;   // faces away: culled
    const bool have_uv = q.uvs.size() == q.pts.size() && q.cols.size() == q.pts.size();
    std::vector<Vx> poly(3);
    const V3* corner[3] = {&a, &b, &cc};
    for (int i = 0; i < 3; ++i) {
      const V3& v = *corner[i];
      for (int j = 0; j < 4; ++j) poly[size_t(i)].c[j] = v[0] * c.vp[0 * 4 + j] + v[1] * c.vp[1 * 4 + j] + v[2] * c.vp[2 * 4 + j] + c.vp[12 + j];
      double* at = poly[size_t(i)].at;
      if (have_uv) {
        const uint32_t cv = q.cols[t + size_t(i)];
        at[0] = q.uvs[t + size_t(i)][0], at[1] = q.uvs[t + size_t(i)][1], at[2] = double((cv >> 24) & 0xFFu);
        at[3] = double(cv & 0xFFu), at[4] = double((cv >> 8) & 0xFFu), at[5] = double((cv >> 16) & 0xFFu);
      } else {
        at[0] = at[1] = 0.5, at[2] = at[3] = at[4] = at[5] = 255.0;
      }
    }
    // clip to the near plane (z >= 0) and the far plane (z <= w) in clip space (attributes are linear in clip space too): Sutherland-Hodgman
    for (int plane = 0; plane < 2 && poly.size() >= 3; ++plane) {
      std::vector<Vx> outp;
      for (size_t i = 0; i < poly.size(); ++i) {
        const Vx& v0 = poly[i];
        const Vx& v1 = poly[(i + 1) % poly.size()];
        const double d0 = plane == 0 ? v0.c[2] : v0.c[3] - v0.c[2], d1 = plane == 0 ? v1.c[2] : v1.c[3] - v1.c[2];
        if (d0 >= 0) outp.push_back(v0);
        if ((d0 >= 0) != (d1 >= 0)) {
          const double f = d0 / (d0 - d1);
          Vx m;
          for (int j = 0; j < 4; ++j) m.c[j] = v0.c[j] + f * (v1.c[j] - v0.c[j]);
          for (int j = 0; j < 6; ++j) m.at[j] = v0.at[j] + f * (v1.at[j] - v0.at[j]);
          outp.push_back(m);
        }
      }
      poly.swap(outp);
    }
    if (poly.size() < 3) continue;
    for (size_t fan = 1; fan + 1 < poly.size(); ++fan) {
      const Vx* V[3] = {&poly[0], &poly[fan], &poly[fan + 1]};
      double sx[3], sy[3], iw[3], dz[3];
      bool ok = true;
      for (int i = 0; i < 3; ++i) {
        if (V[i]->c[3] <= 1e-6) { ok = false; break; }
        sx[i] = (V[i]->c[0] / V[i]->c[3] * 0.5 + 0.5) * w, sy[i] = (V[i]->c[1] / V[i]->c[3] * 0.5 + 0.5) * h, iw[i] = 1.0 / V[i]->c[3], dz[i] = V[i]->c[2] / V[i]->c[3];
      }
      if (!ok) continue;
      const double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
      if (std::fabs(area) < 1e-9) continue;
      const int x0 = std::max(0, int(std::floor(std::min({sx[0], sx[1], sx[2]})))), x1 = std::min(w - 1, int(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
      const int y0 = std::max(0, int(std::floor(std::min({sy[0], sy[1], sy[2]})))), y1 = std::min(h - 1, int(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
      const auto inside = [&](double e, double dx, double dy) {   // top-left rule: an edge shared by two triangles belongs to one of them
        if (area < 0) e = -e, dx = -dx, dy = -dy;
        return e > 0 || (e == 0 && (dy > 0 || (dy == 0 && dx < 0)));
      };
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const double px = x + 0.5, py = y + 0.5;
          const double e0 = (sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px - sx[0]);
          const double e1 = (sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px - sx[1]);
          const double e2 = (sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px - sx[2]);
          if (!inside(e0, sx[1] - sx[0], sy[1] - sy[0]) || !inside(e1, sx[2] - sx[1], sy[2] - sy[1]) || !inside(e2, sx[0] - sx[2], sy[0] - sy[2])) continue;
          const double l0 = e1 / area, l1 = e2 / area, l2 = e0 / area;   // screen-space barycentrics of corners 0, 1, 2
          const double s = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
          if (s <= 0) continue;
          const double z = l0 * dz[0] + l1 * dz[1] + l2 * dz[2];   // z / w is affine in screen space
          if (z < 0 || z > 1) continue;
          double at[6];
          for (int k = 0; k < 6; ++k) at[k] = (l0 * iw[0] * V[0]->at[k] + l1 * iw[1] * V[1]->at[k] + l2 * iw[2] * V[2]->at[k]) / s;
          // the picture lands only where uv0 is inside [0, 1] (outside it the sampler gives no picture: a53's banner quads draw a strip)
          if (have_uv && (at[0] < -1e-4 || at[0] > 1 + 1e-4 || at[1] < -1e-4 || at[1] > 1 + 1e-4)) continue;
          // the game's alpha test: GEQUAL 1 / 255 on the interpolated vertex alpha (a fade to 0 is discarded: no colour, no depth write)
          if (at[2] < 1.0 && g_expect_plant != 3) continue;
          const size_t i = size_t(y) * size_t(w) + size_t(x);
          if (is_mine) mine[i] = 1;
          if (depth_rev ? (z < zb[i] - 1e-7) : (z > zb[i] + 1e-7)) continue;   // the depth test: an earlier, nearer fragment hides this one
          zb[i] = z;
          const double al = std::min(1.0, at[2] / 255.0);
          for (int ch = 0; ch < 3; ++ch) S[i * 3 + size_t(ch)] = al * std::min(1.0, std::max(0.0, at[3 + ch] / 255.0)) + (1.0 - al) * S[i * 3 + size_t(ch)];
          T[i] *= 1.0 - al;
          if (is_mine) hit[i] = 1;
        }
    }
  }
  if (vcol) vcol->assign(npx * 3, 0);
  if (valpha) valpha->assign(npx, 0);
  uint64_t n = 0, hid = 0;
  for (size_t i = 0; i < npx; ++i) {
    if (!hit[i]) {
      if (mine[i]) ++hid;
      continue;
    }
    ++n;
    const double A = 1.0 - T[i];
    if (valpha) (*valpha)[i] = uint8_t(std::min(255.0, std::max(0.0, std::round(A * 255.0))));
    if (vcol)
      for (int ch = 0; ch < 3; ++ch) (*vcol)[i * 3 + size_t(ch)] = uint8_t(std::min(255.0, std::max(0.0, std::round(A > 0 ? S[i * 3 + size_t(ch)] / A * 255.0 : 255.0))));
  }
  if (hidden) *hidden = hid;
  return n;
}

// A viewer camera -> the game-space camera of a Live record / frame_rig --camera (BuildFrame converts it back: row 1 and the eye's y negate).
inline void ToGame(const Cam& c, float vp[16], float eye[3]) {
  for (int i = 0; i < 16; ++i) vp[i] = (i >= 4 && i < 8) ? -c.vp[i] : c.vp[i];
  eye[0] = float(c.eye[0]), eye[1] = float(-c.eye[1]), eye[2] = float(c.eye[2]);
}

}  // namespace wgs
