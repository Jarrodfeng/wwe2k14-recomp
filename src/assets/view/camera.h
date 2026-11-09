// Orbit camera + presets. Matrices are row-major, row-vector convention (clip = pos * View * Proj), left-handed.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace view {

// The game's data is Y-up (a wrestler's height is +Y). The single constant: 1 = Y up, 2 = Z up.
constexpr int kUpAxis = 1;

struct Mat4 { float m[16]; };
struct V3 { float x, y, z; };

inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Norm(V3 a) {
  float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
  if (l < 1e-20f) return {0, 0, 0};
  return {a.x / l, a.y / l, a.z / l};
}
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Camera {
  float target[3] = {0, 0, 0};
  float yaw = 0, pitch = 15, dist = 400, fov = 40;  // degrees; fov is vertical
  // Session 97: an explicit view-projection (viewer space, row vectors: clip = (pos,1) * vp) and eye, e.g. from a game draw probe.
  bool use_vp = false;
  float vp[16] = {0};
  float eye[3] = {0, 0, 0};
  // Session 98 (freecam): a free pose. When `free` is set the camera sits at fpos looking along (fyaw, fpitch) with froll and `fov`
  // (the orbit fields stay as they were; leaving free mode sets the orbit target in front of the camera). use_vp wins over free.
  bool free = false;
  float fpos[3] = {0, 0, 0};
  float fyaw = 0, fpitch = 0, froll = 0;  // degrees; yaw 0 looks along +Z (+Y up frame), yaw up = turn right, pitch up = look up
};

constexpr float kDeg = 3.14159265358979f / 180.0f;

// Place an (up, h1, h2) triple into x,y,z according to kUpAxis.
inline V3 FromUpFrame(float up, float h1, float h2) {
  if (kUpAxis == 1) return {h1, up, h2};
  return {h1, h2, up};
}
inline V3 UpVector() { return FromUpFrame(1, 0, 0); }

// The view direction / right / up of a free pose (degrees; roll > 0 = the right side dips).
inline V3 FreeForward(float yaw, float pitch) {
  float p = pitch * kDeg, y = yaw * kDeg;
  return FromUpFrame(std::sin(p), std::cos(p) * std::sin(y), std::cos(p) * std::cos(y));
}
inline V3 FreeRight(float yaw, float pitch) {  // level, to the camera's right (left-handed: right = up x forward)
  return Norm(Cross(UpVector(), FreeForward(yaw, pitch)));
}
inline V3 FreeUp(float yaw, float pitch, float roll) {
  V3 f = FreeForward(yaw, pitch), r = FreeRight(yaw, pitch);
  V3 u = Cross(f, r);
  float cr = std::cos(roll * kDeg), sr = std::sin(roll * kDeg);
  return {u.x * cr + r.x * sr, u.y * cr + r.y * sr, u.z * cr + r.z * sr};
}

inline V3 Eye(const Camera& c) {
  if (c.use_vp) return {c.eye[0], c.eye[1], c.eye[2]};
  if (c.free) return {c.fpos[0], c.fpos[1], c.fpos[2]};
  float p = c.pitch * kDeg, y = c.yaw * kDeg;
  V3 o = FromUpFrame(std::sin(p) * c.dist, std::cos(p) * std::sin(y) * c.dist, std::cos(p) * std::cos(y) * c.dist);
  return {c.target[0] + o.x, c.target[1] + o.y, c.target[2] + o.z};
}

inline Mat4 LookAt(V3 eye, V3 at, V3 up) {
  V3 z = Norm(at - eye), x = Norm(Cross(up, z)), y = Cross(z, x);
  Mat4 r = {{x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1}};
  return r;
}
inline Mat4 Perspective(float fov_deg, float aspect, float n, float f) {
  float ys = 1.0f / std::tan(fov_deg * kDeg * 0.5f), xs = ys / aspect, q = f / (f - n);
  Mat4 r = {{xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, 1, 0, 0, -n * q, 0}};
  return r;
}
inline Mat4 Mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
      r.m[i * 4 + j] = s;
    }
  return r;
}

// Near / far from the camera distance and the scene size.
inline void ClipPlanes(const Camera& c, float scene_diag, float& n, float& f) {
  n = std::max(0.5f, c.dist * 0.01f);
  f = std::max(c.dist + scene_diag * 2.0f, n * 10.0f);
}

inline Mat4 ViewProj(const Camera& c, float aspect, float scene_diag) {
  if (c.use_vp) { Mat4 r; std::memcpy(r.m, c.vp, sizeof r.m); return r; }
  float n, f;
  ClipPlanes(c, scene_diag, n, f);
  if (c.free) {  // free: a fixed near plane; the far plane also covers the way from the eye to the orbit target (a point in the scene)
    f += std::sqrt((c.fpos[0] - c.target[0]) * (c.fpos[0] - c.target[0]) + (c.fpos[1] - c.target[1]) * (c.fpos[1] - c.target[1]) +
                   (c.fpos[2] - c.target[2]) * (c.fpos[2] - c.target[2]));
    n = std::max(1.0f, f * 1e-4f);  // (keeps near / far at most 1e4, so the far crowd keeps its depth precision)
    V3 e = {c.fpos[0], c.fpos[1], c.fpos[2]}, fw = FreeForward(c.fyaw, c.fpitch);
    return Mul(LookAt(e, {e.x + fw.x, e.y + fw.y, e.z + fw.z}, FreeUp(c.fyaw, c.fpitch, c.froll)), Perspective(c.fov, aspect, n, f));
  }
  V3 t = {c.target[0], c.target[1], c.target[2]};
  return Mul(LookAt(Eye(c), t, UpVector()), Perspective(c.fov, aspect, n, f));
}

inline const char* kPresetNames[8] = {"front", "side", "three_quarter", "top", "close", "ring", "ring_side", "ring_back"};

// Presets from the model bounds. Returns false for an unknown name.
inline bool ApplyPreset(Camera& c, const std::string& name, const float bmin[3], const float bmax[3]) {
  bool known = false;
  for (const char* n : kPresetNames) known |= name == n;
  if (!known) return false;  // (checked first: an unknown name must not move the camera)
  c.use_vp = false;
  float ctr[3], ext[3];
  for (int i = 0; i < 3; i++) {
    ctr[i] = (bmin[i] + bmax[i]) * 0.5f;
    ext[i] = bmax[i] - bmin[i];
  }
  int U = kUpAxis;
  float h = std::max(ext[U], 1.0f);
  float diag = std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
  float r = std::max(diag * 0.5f, 1.0f);
  c.fov = 40;
  float fit = r / std::tan(c.fov * kDeg * 0.5f) * 1.05f;
  for (int i = 0; i < 3; i++) c.target[i] = ctr[i];
  // (Session 96: models face -Z after the loader's -Y-up -> +Y-up mirror, so "front" looks from yaw 180)
  if (name == "front") { c.yaw = 180; c.pitch = 5; c.dist = fit; }
  else if (name == "side") { c.yaw = 270; c.pitch = 5; c.dist = fit; }
  else if (name == "three_quarter") { c.yaw = 215; c.pitch = 18; c.dist = fit; }
  else if (name == "top") { c.yaw = 0; c.pitch = 89; c.dist = fit; }
  else if (name == "close") {
    c.target[U] = bmin[U] + h * 0.85f;
    c.yaw = 200; c.pitch = 5; c.dist = h * 0.3f;
  } else if (name == "ring" || name == "ring_side" || name == "ring_back") {
    // (Session 96: ring_side / ring_back = the same shot turned 90 / 180 degrees, for the census's inside views)
    // arenas: from 30% of the way out (of the larger horizontal half-width), looking at the origin
    int h1 = 0, h2 = (U == 1) ? 2 : 1;
    float half = std::max(ext[h1], ext[h2]) * 0.5f;
    c.target[0] = c.target[1] = c.target[2] = 0;
    c.target[U] = std::min(std::max(0.0f, bmin[U]) + h * 0.02f, bmax[U]);
    c.yaw = name == "ring" ? 0.0f : name == "ring_side" ? 90.0f : 180.0f;
    // (Session 96 census: 30% of the half-width put the camera 4,000+ units out in the big arenas, often inside the set;
    // a fixed ringside distance works for all. The ring is about 600 units across.)
    // (Session 97: at least 1000 - arena 23 is a ring alone in a small set, and 30% of its half-width framed the skirt)
    c.pitch = 12; c.dist = std::min(std::max(half * 0.3f, 1000.0f), 1600.0f);
  } else return false;
  return true;
}

}  // namespace view
