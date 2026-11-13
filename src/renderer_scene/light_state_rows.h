// The per-draw light constant rows from a light state (Session 101, lane 109 = LS3; docs\renderer\PER_DRAW_LIGHTS.md sections 1.1, 1.3, 3). Pure: a
// LightState (scene_record.h) -> the rows the game's packer / binder put in the constant banks, so a D3 draw (the game's work skipped, no stream draw)
// can get them without the stream. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game or SDK includes.
//
// Marks: [P] proven bit-exact against the stream's banks on Check B (run 7051) and run 7079 by tools\light\light_states_selftest.py (lane 101 measured
// the rules, lane 109 re-proves them through this code); [L] a reading of the packer's formula, not yet proven (the proof waits on a capture that carries
// the light tables: the ledger's `light_state` column).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "scene_record.h"

namespace rdr_scene {

struct LightRows {
  float dif_lgh_vec[5][4];    // [P] VS c8-c10 (c8-c12 in the PS): light k's direction, R+16+16k as stored (w included)
  float dif_lgh_col[5][4];    // [P] VS c0-c3 / PS c0-c4: light k's colour rgbw, R+96+16k (w = the lobe weight)
  float key_light_vec[4];     // [P] VS c12 (wrestlers) = light 4's direction
  float key_light_color[4];   // [P] VS c4 (wrestlers) = light 4's colour
  float dif_lgh_max[4];       // [P] PS c197 = the float32 sum of rows 0-3 of dif_lgh_col, added in order 0+1, +2, +3
  float dif_lgh_max_a[4];     // [P] PS c199 = (m, 1/m, 0, 0), m = the largest of the four sum components (m == 0: (0, 0, 0, 0), [U] the game's value)
  float block_a[4];           // [L] VS / PS c181 or c182 (SkyCol / GroundCol): R+352; which register is which is [U] (PER_DRAW_LIGHTS.md 5.2)
  float block_b[4];           // [L] the other: R+368
  bool tex_proj_valid;        // false when the state's shadow-map size is unknown (kLsMapKnown unset): tex_proj_light stays zero
  float tex_proj_light[4][4]; // [L] VS c68-c71 = the columns of bias(W, H) x M(R+1040) x M(R+912) (register r = column r), float32
  float shadow_prm[3][4];     // [L] PS c168-c170: R floats (+1108, +1112, +1104, +1120), (+1128, +1136, +1140, +1144), (+1148, +1152, +1156, +1160); which row is which: [U]
  float shadow_view_t[4][4];  // [L] transpose(M(R+912)): the packer's S+96 block; LightEye / ViewLightDepth (VS c166) match a row of a shadow ViewProj within 1e-6 [C]
  float light_view_t[4][4];   // [L] transpose(M(R+976)): the packer's S+352 block
  float shadow_x[4][4];       // [L] R+2144 (2 rows) and R+2176 (2 rows): the packer's S+(15+s)x32 / S+(17+s)x32 blocks
};

namespace light_rows_detail {
inline void Mul4(const float a[16], const float b[16], float out[16]) {  // row-vector convention, float32, summed in k order
  float r[16];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      volatile float p = a[i * 4] * b[j];  // volatile: a float32 rounding after every operation (no fused multiply-add, whatever the build flags)
      float s = p;
      for (int k = 1; k < 4; ++k) {
        p = a[i * 4 + k] * b[k * 4 + j];
        s = s + p;
      }
      r[i * 4 + j] = s;
    }
  std::memcpy(out, r, sizeof r);
}
inline void Transpose4(const float a[16], float out[4][4]) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[j][i] = a[i * 4 + j];
}
}  // namespace light_rows_detail

inline LightRows BuildLightRows(const LightState& s) {
  using namespace light_rows_detail;
  LightRows r;
  std::memset(&r, 0, sizeof r);
  for (int k = 0; k < 5; ++k) {
    std::memcpy(r.dif_lgh_vec[k], &s.light[kLsDir + 4 * k], 16);
    std::memcpy(r.dif_lgh_col[k], &s.light[kLsCol + 4 * k], 16);
  }
  std::memcpy(r.key_light_vec, r.dif_lgh_vec[4], 16);
  std::memcpy(r.key_light_color, r.dif_lgh_col[4], 16);
  float sum[4];
  for (int c = 0; c < 4; ++c) {
    float a = r.dif_lgh_col[0][c];
    volatile float t = a + r.dif_lgh_col[1][c];  // volatile: float32 rounding at each add, whatever the compiler's contraction / precision mode
    t = t + r.dif_lgh_col[2][c];
    t = t + r.dif_lgh_col[3][c];
    sum[c] = t;
  }
  std::memcpy(r.dif_lgh_max, sum, 16);
  float m = std::max(std::max(sum[0], sum[1]), std::max(sum[2], sum[3]));
  m = std::max(m, 0.f);
  r.dif_lgh_max_a[0] = m;
  r.dif_lgh_max_a[1] = m > 0.f ? 1.f / m : 0.f;
  std::memcpy(r.block_a, &s.light[kLsBlockA], 16);
  std::memcpy(r.block_b, &s.light[kLsBlockB], 16);
  // TexProjLight: transpose(bias(W, H) x M1040 x M912)
  if ((s.flags & kLsMapKnown) && s.map_wh[0] > 0.f && s.map_wh[1] > 0.f) {
    const float w = s.map_wh[0], h = s.map_wh[1];
    const float bias[16] = {0.5f, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 1.f, 0, 0.5f + 0.5f / w, 0.5f - 0.5f / h, 0, 1.f};
    float p1[16], p2[16];
    Mul4(bias, s.shadow_m[2], p1);
    Mul4(p1, s.shadow_m[0], p2);
    Transpose4(p2, r.tex_proj_light);
    r.tex_proj_valid = true;
  }
  const float* f = s.shadow_f;  // f[i] = R float at byte 1104 + 4i
  auto F = [&](int byte) { return f[(byte - 1104) / 4]; };
  const float prm[3][4] = {{F(1108), F(1112), F(1104), F(1120)}, {F(1128), F(1136), F(1140), F(1144)}, {F(1148), F(1152), F(1156), F(1160)}};
  std::memcpy(r.shadow_prm, prm, sizeof prm);
  Transpose4(s.shadow_m[0], r.shadow_view_t);
  Transpose4(s.shadow_m[1], r.light_view_t);
  std::memcpy(r.shadow_x, s.shadow_x, sizeof r.shadow_x);
  return r;
}

// ---- the caster passes' camera rows (Session 101, lane 211 = Caster D3 E3; docs\renderer\SKIP_GAME_WORK_MAP.md 17.8) ----------------------------------------------------------------------------
// The rows a caster pass (map T_ShadowMap / plane T_PlaneShadowMap / rim T_RimLightMap) reads from the render camera record R, built from the state a pass ENTRY read (kScCasterStates; it is a LightState: the same
// used ranges of R). Which R that is at the entry (the colour call's, or the light's A / B camera) is [U] (17.2 item 2): this function does not care, it maps whatever state it is given; the caller
// (d3_record.h CasterCamSource) chooses the state.
// Marks: [P] proven against the stream's banks of the kept J1 capture (run 7086, frame 200) by tools\caster\caster_cam_check.py (the plane draws 40 of 40; the props map's c56-59 and c54 on the 11 props-map
// draws; see 17.8); [L] a reading; [U] unknown.
struct CasterRows {
  float view_proj[4][4];       // [P props map] VS c56-c59: register c56 + i = COLUMN i of M(R+912) (shadow_m[0]) as stored (m[4 r + i] over r)
  float view_depth[4];         // [P] VS c54 = c59 (ViewProj register row 3) bit for bit (yShadowMap only)
  float trapezoid[4][4];       // [P plane] VS c64-c67: c64-c66 = columns 0-2 of M(R+1040) (shadow_m[2]), c67 = column 3 of M(R+976) (shadow_m[1]). The wrestler maps' c64 / c65 equal no colour-call state [C J1 f200: 0 of 93 / 11 of 93]
  float shadow_prm[3][4];      // [L] PS c168-c170 (yShadowMap fragment reads c168 only), the same rows as LightRows::shadow_prm
  bool rim_cam_valid;          // lane 221: view_proj / view_depth came from the entry's CasterExtra product (the thread slot's CURRENT camera), not from R (ApplyCasterExtra); set for the rim pass only
  bool rim_light_depth_valid;  // lane 221: true once ApplyCasterExtra read G+300 / G+304 (kCxDepth). Before that (lane 211): false always, c162 is (near, far, 0, 0) and equals NO field of R: c162 (RimLightDepth, yRimLightMap) is (near, far, 0, 0) of the rim camera and equals NO field of R [P: J1 f100 / f200 / f300: 454.26 / 647.61, 275.15 / 494.70, 909.95 / 1513.36 against shadow_f]
  float rim_light_depth[4];
};
inline CasterRows BuildCasterRows(const LightState& s) {
  CasterRows r;
  std::memset(&r, 0, sizeof r);
  const float* m0 = s.shadow_m[0];
  const float* m1 = s.shadow_m[1];
  const float* m2 = s.shadow_m[2];
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) r.view_proj[i][k] = m0[k * 4 + i];
  std::memcpy(r.view_depth, r.view_proj[3], 16);
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 4; ++k) r.trapezoid[i][k] = m2[k * 4 + i];
  for (int k = 0; k < 4; ++k) r.trapezoid[3][k] = m1[k * 4 + 3];
  const float* f = s.shadow_f;
  auto F = [&](int byte) { return f[(byte - 1104) / 4]; };
  const float prm[3][4] = {{F(1108), F(1112), F(1104), F(1120)}, {F(1128), F(1136), F(1140), F(1144)}, {F(1148), F(1152), F(1156), F(1160)}};
  std::memcpy(r.shadow_prm, prm, sizeof prm);
  return r;
}

// ---- the wrestlers' light camera rows (Session 101, lane 227 = Wrestlers D3 H2; docs\renderer\SKIP_GAME_WORK_MAP.md 18.7) ------------------------------------------------------------------------------
// A wrestler's COLOUR draw reads VS TexProjLight c68-c71 and LightEye c166 from the wrestler light camera (maps A / B), which the colour call's own light state does NOT hold (0 of 1920 reads match any state row,
// 18.1). The camera is the state the caster MAP pass entry read (kScCasterStates: H0, shared with E3). [C: lane 227, run 7147 (z7147 capture), 60 of 60 sampled wrestler-frames, tools\d3\d3_wrestler_check.py
// `camera` bar, scratch probe numbers in 18.7] the four registers equal, BIT FOR BIT in float32, TRANSPOSE((M(R+912) x M(R+1040)) x bias(W, H)) with the row-vector product of Mul4 (k ascending, a float32 rounding after
// every operation; a fused multiply-add gives the same bits), i.e. the multiplication order is the REVERSE of the arena's (bias x M1040 x M912, BuildLightRows). c166 = column 3 of M(R+912) (ViewProj register row 3, as
// BuildCasterRows' view_proj[3]). Every map entry of a hook frame read the same camera in the sampled frames (one light camera per frame); the caller takes the first.
struct WrestlerCameraRows {
  bool valid = false;            // false when the state's shadow-map size is unknown (kLsMapKnown unset)
  float tex_proj_light[4][4] = {};   // VS c68..c71
  float light_eye[4] = {};           // VS c166 (= ViewProj row 3)
};
inline WrestlerCameraRows BuildWrestlerCameraRows(const LightState& s) {
  using namespace light_rows_detail;
  WrestlerCameraRows r;
  if (!((s.flags & kLsMapKnown) && s.map_wh[0] > 0.f && s.map_wh[1] > 0.f)) return r;
  const float w = s.map_wh[0], h = s.map_wh[1];
  const float bias[16] = {0.5f, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 1.f, 0, 0.5f + 0.5f / w, 0.5f - 0.5f / h, 0, 1.f};
  float p1[16], p2[16];
  Mul4(s.shadow_m[0], s.shadow_m[2], p1);
  Mul4(p1, bias, p2);
  Transpose4(p2, r.tex_proj_light);
  for (int k = 0; k < 4; ++k) r.light_eye[k] = s.shadow_m[0][k * 4 + 3];
  r.valid = true;
  return r;
}

// lane 221 (SKIP_GAME_WORK_MAP.md 17.10): the rim pass's camera and depth range. The rim pass renders from the MAIN camera, which is the thread slot's CURRENT camera (the product table at 0x839299C0 + 64 * slot,
// sub_82BA7718) and NOT in R (R+912 holds the shadow light's camera): the stream's rim c56-59 equal the colour draws' c56-59 of the same frame on every draw and equal no word of any record before this
// section [C: run 7147's zip, tools\caster\caster_extras_check.py]. The twin packs the product transposed (lvx128 x4, vmrghw/vmrglw, stvx128: 0x82BD8C24-0x82BD8C64), so register c56 + i = COLUMN i of the
// product matrix as stored (p[4 r + i] over r), the same rule as M(R+912). c162 = (float [G+300], float [G+304], 0, 0) (0x82BD8B14-0x82BD8B88). Only the rim pass takes the product (`rim` true); the other passes keep R.
inline void ApplyCasterExtra(CasterRows& r, const CasterExtra& x, bool rim) {
  if (rim && (x.flags & kCxProduct)) {
    for (int i = 0; i < 4; ++i)
      for (int k = 0; k < 4; ++k) r.view_proj[i][k] = x.product[k * 4 + i];
    std::memcpy(r.view_depth, r.view_proj[3], 16);
    r.rim_cam_valid = true;
  }
  if (rim && (x.flags & kCxDepth)) {
    r.rim_light_depth[0] = x.depth[0], r.rim_light_depth[1] = x.depth[1], r.rim_light_depth[2] = 0.f, r.rim_light_depth[3] = 0.f;
    r.rim_light_depth_valid = true;
  }
}

}  // namespace rdr_scene
