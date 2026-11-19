// S103 V1: the arena's stage visibility in OUR frame (docs\formats\stage_visibility.md). Header only, no game / SDK / GPU includes.
//
// The game keeps a box per stage-slot model (pac entry 50020, a tree of named AABBs) and rejects a model whose box lies wholly behind one of six planes of the "stage frustum"
// (sub_82769820: d = n.(c - P) <= -half rejects; sub_82699C60 asks it per tree, sub_824E9E10 draws a slot only if its copy passes). We own the camera, so the volume is built
// from OUR ViewProj and our aspect, and the test is the same one:
//   - y: the display's own half height (tan theta = 1 / P11);
//   - x: the game's rule widens the half ANGLE by the screen-mode constant (sub_82769B98: tan(theta x 16/9) at the 16:9 mode), never narrower than the display's own x: a wider
//     screen sees more (no 16:9 limit, the S101 lane 234 hook's job done structurally);
//   - kSlack: an empirical 5 % margin of ours (the volume is rebuilt from the display matrix, not from the game's camera words; misses of 0.4 % measured; 2 % was 0.1 % short).
// Conventions (the record's Mat4, front_end.cpp): row vectors, clip = [p 1] * View * Proj, D3D depth 0..w, boxes in the VIEWER's space (the loader's Y mirror).
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace rdr_scene::stagevis {

inline std::atomic<uint64_t> g_applies{0}, g_cull_changes{0};   // arena state re-applies (rf_arena.cpp) / frames whose rejected set changed: the cull must leave g_applies alone

inline constexpr float kStageTerm16x9 = 16.0f / 9.0f;   // the constant at 0x8202B710 [C: image read, docs\formats\stage_visibility.md]
inline constexpr float kSlack = 1.05f;

struct Planes {
  float p[6][4] = {};   // unit normal n and d: inside when n.p + d >= 0
  bool ok = false;
};

// plant_no_widen: the x term of the stage volume is dropped (the plain display frustum): the twin that must lose slots the game draws.
inline Planes StagePlanes(const float view[16], const float proj[16], float term = kStageTerm16x9, float slack = kSlack, bool plant_no_widen = false) {
  float m[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += view[r * 4 + k] * proj[k * 4 + c];
      m[r * 4 + c] = s;
    }
  Planes out;
  const auto colnorm = [&](int c) { return std::sqrt(m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c]); };   // |column c| over x y z = the projection's scale (the view is a rotation)
  const float p00 = colnorm(0), p11 = colnorm(1);
  if (!(p00 > 1e-9f) || !(p11 > 1e-9f) || !std::isfinite(p00) || !std::isfinite(p11)) return out;
  float kx = std::tan(std::atan(1.0f / p11) * term) * p00;   // the stage's tan_x over the display's tan_x
  if (!(kx > 1.f) || plant_no_widen) kx = 1.f;
  for (int i = 0; i < 4; ++i) {   // columns 0 and 1 carry the clip x and y: dividing them widens the volume in that axis only
    m[i * 4 + 0] /= kx * slack;
    m[i * 4 + 1] /= slack;
  }
  const auto col = [&](int c, int i) { return m[i * 4 + c]; };
  for (int i = 0; i < 4; ++i) {
    out.p[0][i] = col(3, i) + col(0, i);   // left
    out.p[1][i] = col(3, i) - col(0, i);   // right
    out.p[2][i] = col(3, i) + col(1, i);   // bottom
    out.p[3][i] = col(3, i) - col(1, i);   // top
    out.p[4][i] = col(2, i);               // near   (z >= 0)
    out.p[5][i] = col(3, i) - col(2, i);   // far    (w - z >= 0)
  }
  for (int k = 0; k < 6; ++k) {
    const float n = std::sqrt(out.p[k][0] * out.p[k][0] + out.p[k][1] * out.p[k][1] + out.p[k][2] * out.p[k][2]);
    if (!(n > 1e-12f) || !std::isfinite(n)) return Planes{};
    for (int i = 0; i < 4; ++i) out.p[k][i] /= n;
  }
  out.ok = true;
  return out;
}

// The game's box reject: false only when the box lies wholly behind one plane. `flip_x` tests the x-mirrored copy (the `r` twin). A frustum that could not be built keeps everything.
inline bool BoxVisible(const Planes& f, const float lo[3], const float hi[3], bool flip_x = false) {
  if (!f.ok) return true;
  float l[3] = {lo[0], lo[1], lo[2]}, h[3] = {hi[0], hi[1], hi[2]};
  if (flip_x) {
    const float a = -hi[0], b = -lo[0];
    l[0] = a, h[0] = b;
  }
  const float c[3] = {(l[0] + h[0]) * .5f, (l[1] + h[1]) * .5f, (l[2] + h[2]) * .5f}, e[3] = {(h[0] - l[0]) * .5f, (h[1] - l[1]) * .5f, (h[2] - l[2]) * .5f};
  for (int k = 0; k < 6; ++k) {
    const float* p = f.p[k];
    const float d = p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3];
    const float half = std::fabs(p[0]) * e[0] + std::fabs(p[1]) * e[1] + std::fabs(p[2]) * e[2];
    if (d <= -half) return false;
  }
  return true;
}

}  // namespace rdr_scene::stagevis
