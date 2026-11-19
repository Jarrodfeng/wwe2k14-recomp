// The wrestlers' shadow light camera, fitted by OUR code from the record (Session 103, lane E3; docs\renderer\LIVE_MAPS.md "Lane E3").
// Pure: no game, no SDK, no renderer. Header only.
//
// Why it exists. The game's wrestler shadow camera (R+912 / +976 / +1040) is the output of the shadow module's fit (sub_827E7838, 365 instructions, fed by the main camera's frustum and
// the module's light descriptor); it is written inside the fit task and the walkers, and the caster-pass entry records that carry it (kScCasterStates) die with the walk (INPUTS_A2.md 5b / 8). So on an
// OURS frame there is no camera in the record. Rule 6: the trapezoid fit over the main camera's frustum is a 360 quality trick, not something to reproduce: this fit builds the same KIND of camera
// (a far eye, a narrow perspective, a trapezoid squeeze that zooms to the casters) from the two things the record can give: the KEY LIGHT VECTOR and the CASTERS' BOUNDS.
//
// What is copied from the game, measured [C] on the stream of capture brock_losswin frame 110 (wrestler colour draw, VS TexProjLightW = ViewLightDepth, KeyLightVec) and z7147 frames 3886-4226
// (caster states, `tools\scene\e3_dump3`), 6 of 6 frames:
//   - the light's travel direction fwd = (-key.x, +0.5150, -key.z) with the horizontal part scaled to sqrt(1 - 0.5150^2): the elevation is a CONSTANT 0.5150: [C] on 7 captures (brock_losswin, brock_s15, brock_tron, s100_wm30_300, s100_match300, s100_s89k_a, z7147; key y from 0.43 to 0.64), [U] elsewhere, the azimuth is the key
//     light's, turned half a circle. brock f110: key (-0.2483, 0.5777, -0.7776) -> fwd (0.2607, 0.5150, 0.8166) vs the stream's (0.2615, 0.5150, 0.8163) (0.05 degrees). [C]
//   - the eye sits kEyeDistance = 59909.99 from the look-at point along -fwd (c166 w = 59909.9883 in brock, z7147 M976 row 3); [C]
//   - the projection scale cot(22.5 degrees) = 2.4142135 (z7147 M912 columns 0 and 1 = 2.4142 x the right / up axes, y flipped); [C]
//   - depth: z_clip = A (vz - near), A = far / (far - near) (z7147: A = 243.75, near 60246.9, far 60495.1: 60495.1 / 248.2 = 243.7); ShadowPrm = (near, far, 4 / (far - near), 0.7): F[0] = 4 / (far - near) in 6
//     of 6 frames; [C]
//   - the trapezoid is a pure zoom + offset on the clip x / y: M1040 = diag(S, S, 1, 1) with the offset in row 3. [C]
// What is NOT copied: the game looks at the world origin and zooms (S up to 140) onto the main camera's frustum slice; this fit looks AT the casters' bounding sphere (offset 0) and zooms to its
// radius, so the map is always as tight as the casters allow. The readers take the matrices from MapImage, so the two ends stay consistent whatever the camera. ShadowPrm rows 1 and 2 (the
// readers' penumbra / bias constants, F[5..14]) are the game's wrestler values: [U] how the game derives them (they changed 4.136 -> 4.368 between captures); they are the readers' to use.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "scene_record.h"

namespace rdr_scene {

inline constexpr float kShadowFitElevationY = 0.5150f;      // [C] the light's travel direction y, every frame of both captures
inline constexpr float kShadowFitEyeDistance = 59909.99f;   // [C] c166 w / M976 row 3 z
inline constexpr float kShadowFitProjScale = 2.4142135f;    // [C] cot(22.5 degrees)
inline constexpr float kShadowFitDepthPad = 20.f;           // world units added to the casters' radius for the depth range (the limbs' thickness beyond a sampled vertex is zero; this covers the sampling)
inline constexpr float kShadowFitWidthPad = 12.f;           // and for the zoom
inline constexpr float kShadowFitMinRadius = 60.f;          // a lone small bound still gets a usable zoom
inline constexpr float kShadowFitMaxScale = 400.f;          // the squeeze never goes above this (the game's own tops out at 140)

struct ShadowFitInput {
  float key_vec[3] = {};       // the wrestlers' KeyLightVec as the colour draw reads it (R dir[4]; VS c12), game space
  float center[3] = {};        // the casters' bounding sphere centre, game space
  float radius = 0.f;          // and its radius (world units)
  uint32_t map_w = 512, map_h = 512;
};

// Planted faults for the proof: each must fail the check that sees it.
struct ShadowFitFaults {
  bool view_not_light = false;   // the caster matrix is built from the main camera's forward vector (the view) instead of the light's: the shadow lands in the wrong place
  bool key_not_negated = false;  // the key vector is used as the travel direction without turning it half a circle: the shadow falls toward the light
  bool no_bounds = false;        // the radius is left at zero (the zoom would be the minimum on every scene)
  bool Any() const { return view_not_light || key_not_negated || no_bounds; }
};

struct ShadowFit {
  LightState state = {};         // shadow_m[0..2], shadow_f, map_wh, flags: the same fields the game's caster state holds (BuildCasterRows / BuildWrestlerCameraRows read them)
  float fwd[3] = {};             // the light's travel direction
  float scale = 0.f;             // the trapezoid squeeze S
  float near_d = 0.f, far_d = 0.f;
};

namespace shadow_fit_detail {
inline void Mul4(const float* a, const float* b, float* out) {   // row-vector product a then b
  float r[16];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = s;
    }
  std::memcpy(out, r, sizeof r);
}
inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
}  // namespace shadow_fit_detail

// Fits the light camera. Returns false (with `why`) when the key vector has no horizontal part or the bounds are not finite. `main_forward` is only read by the view_not_light plant (the main camera's forward, game space).
inline bool FitShadowLight(const ShadowFitInput& in, ShadowFit& out, const char*& why, const ShadowFitFaults& faults = ShadowFitFaults(), const float* main_forward = nullptr) {
  using namespace shadow_fit_detail;
  out = ShadowFit();
  if (!std::isfinite(in.key_vec[0]) || !std::isfinite(in.key_vec[2]) || !std::isfinite(in.center[0]) || !std::isfinite(in.center[1]) || !std::isfinite(in.center[2]) || !std::isfinite(in.radius)) {
    why = "the key light or the casters' bounds are not finite";
    return false;
  }
  if (in.map_w < 64 || in.map_h < 64 || in.map_w > 4096 || in.map_h > 4096) {
    why = "the map size is out of range";
    return false;
  }
  const float sign = faults.key_not_negated ? 1.f : -1.f;
  float hx = sign * in.key_vec[0], hz = sign * in.key_vec[2];
  if (faults.view_not_light && main_forward) hx = main_forward[0], hz = main_forward[2];
  const float hl = std::sqrt(hx * hx + hz * hz);
  if (hl < 1e-3f) {
    why = "the key light points straight up or down: no azimuth";
    return false;
  }
  const float hs = std::sqrt(1.f - kShadowFitElevationY * kShadowFitElevationY) / hl;
  float fwd[3] = {hx * hs, kShadowFitElevationY, hz * hs};
  if (faults.view_not_light && main_forward) {   // the plant: the main camera's whole forward vector, elevation included, becomes the light's
    const float ml = std::sqrt(Dot(main_forward, main_forward));
    if (ml > 1e-3f) fwd[0] = main_forward[0] / ml, fwd[1] = main_forward[1] / ml, fwd[2] = main_forward[2] / ml;
  }
  // axes as the game's M976 columns hold them: right horizontal, up = fwd x right
  const float rl = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
  if (rl < 1e-3f) {
    why = "the light's travel direction is vertical";
    return false;
  }
  const float right[3] = {fwd[2] / rl, 0.f, -fwd[0] / rl};
  const float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
  const float radius = faults.no_bounds ? 0.f : std::max(in.radius, 0.f);
  const float r_fit = std::max(radius, kShadowFitMinRadius);
  const float D = kShadowFitEyeDistance;
  const float near_d = D - (r_fit + kShadowFitDepthPad), far_d = D + (r_fit + kShadowFitDepthPad);
  const float A = far_d / (far_d - near_d);
  // view: columns right, up, fwd; the casters' centre sits at view z = D
  float V[16] = {right[0], up[0], fwd[0], 0.f, right[1], up[1], fwd[1], 0.f, right[2], up[2], fwd[2], 0.f,
                 -Dot(in.center, right), -Dot(in.center, up), D - Dot(in.center, fwd), 1.f};
  const float k = kShadowFitProjScale;
  const float P[16] = {k, 0, 0, 0, 0, -k, 0, 0, 0, 0, A, 1.f, 0, 0, -A * near_d, 0};
  float M912[16];
  Mul4(V, P, M912);
  const float S = std::min(kShadowFitMaxScale, D / (k * (r_fit + kShadowFitWidthPad)));
  const float M1040[16] = {S, 0, 0, 0, 0, S, 0, 0, 0, 0, 1.f, 0, 0, 0, 0, 1.f};
  LightState& s = out.state;
  std::memcpy(s.shadow_m[0], M912, sizeof M912);
  std::memcpy(s.shadow_m[1], V, sizeof V);
  std::memcpy(s.shadow_m[2], M1040, sizeof M1040);
  s.shadow_f[0] = 4.f / (far_d - near_d), s.shadow_f[1] = near_d, s.shadow_f[2] = far_d, s.shadow_f[3] = 0.f, s.shadow_f[4] = 0.7f;
  // the readers' rows (the game's wrestler values: brock f110 and z7147 f100 agree to the second digit) [U: derivation]
  const float rest[10] = {1.0f, 0.3f, 1.8f, 0.2f, 4.2f, 4.0f, 0.067f, 1.67f, 20.0f, 1.0f};
  for (int i = 0; i < 10; ++i) s.shadow_f[5 + i] = rest[i];
  s.map_wh[0] = float(in.map_w), s.map_wh[1] = float(in.map_h);
  s.flags = kLsMapKnown;
  std::memcpy(out.fwd, fwd, sizeof fwd);
  out.scale = S, out.near_d = near_d, out.far_d = far_d;
  why = "";
  return true;
}

}  // namespace rdr_scene
