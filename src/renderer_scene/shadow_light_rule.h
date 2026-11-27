// The wrestlers' shadow light direction: the GAME's rule, ported (Session 111, SHADOWKEY; docs\systems\shadows.md "The wrestlers' shadow light
// direction: the game's rule"). Pure: no game, no SDK, no renderer. Header only.
//
// The game's light fit (sub_827E7838, every frame from the shadow module's task) builds the wrestlers' light from the module's descriptor, never
// from the key light:
//   1. a rotation R from the angle vector (Heading, Pitch, 0) (sub_82C330D8: the rows of Rx Ry Rz for row vectors); the travel direction is
//      (0, 0, 1) x R = row 2 of R;
//   2. when the descriptor's SyncWithCamera word is set and the renderer has a current view: c = (0, 0, -1) x (the view's world -> view 3x3)
//      and the direction is turned about Y by theta = atan2(c.z, c.x) (sub_82C32DE0: row 0 = (cos, 0, -sin), row 2 = (sin, 0, cos));
//   3. normalised.
// Heading / Pitch / SyncWithCamera are the stage shadow table's (CShadowData, misc02_ingame.pac leaf 101, applied per arena id by
// sub_827F40D0): all 100 rows (ids 0..99) hold Heading -149, Pitch -90, SyncWithCamera 1, so this is one rule for every arena: elevation
// y = -sin(-149 deg) = 0.5150 (game space is Y down: the light travels down) and the horizontal part 0.8572 toward (-V[2][0], V[2][2]) normalised: the
// camera's forward heading for a level camera; a pitched camera's leans by its cos pitch (z7147 record 4053: 0.8 degrees from the forward's heading).
// [L] Not covered by the constants: a module whose table row was never applied keeps the initialiser's -140 / -90 (sub_827E6F88), and the motion
// handlers that clear SyncWithCamera (kStageShadowSyncWithCamera below).
#pragma once
#include <cmath>

namespace rdr_scene {

inline constexpr float kStageShadowHeadingDeg = -149.f;     // the table's <Heading>, every row
inline constexpr float kStageShadowPitchDeg = -90.f;        // <Pitch>, every row
inline constexpr bool kStageShadowSyncWithCamera = true;    // <SyncWithCamera>, every row. [L] Three handlers can clear the module's word (sub_823F6B78 code 207, sub_82417AA0
                                                            // event 136, sub_8272F190 camera command 136): 0 of 35,173 mot00..08 motions carry 207, no entrance rig command is 136
                                                            // [C data]; the motion camera's commands and dlc_mot01 are not counted (docs\systems\shadows.md).
inline constexpr float kShadowDegToRad = 0.017453292f;      // the game's own factor (sub_827F40D0)

// The two terms of the main camera's world -> view V the rule reads, V[2][0] and V[2][2], from its row-vector view x projection `vp` (the
// game's perspective: P[0][0] > 0 and clip w = view z, so VP[2][3] = V[2][2], and column 0 of VP is P[0][0] times the view's unit right axis).
// False when the matrix is not finite or has no column 0 (no usable camera).
inline bool ShadowRuleViewTerms(const float vp[16], float& v20, float& v22) {
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(vp[i])) return false;
  const float p00 = std::sqrt(vp[0] * vp[0] + vp[4] * vp[4] + vp[8] * vp[8]);
  if (!(p00 > 1e-6f)) return false;
  v20 = vp[8] / p00, v22 = vp[11];
  return true;
}

// The light's travel direction, unit length, game space. `have_view` false = the renderer has no current view: the descriptor's own
// direction, not turned (the game's branch when the render view is null).
inline void ShadowLightTravelDir(float v20, float v22, bool have_view, float out[3]) {
  const float h = kStageShadowHeadingDeg * kShadowDegToRad, p = kStageShadowPitchDeg * kShadowDegToRad;
  const float sx = std::sin(h), cx = std::cos(h), sy = std::sin(p), cy = std::cos(p);   // the third angle is 0: sin 0, cos 1
  float d[3] = {cx * sy, -sx, cy * cx};                                                   // row 2 of R(Heading, Pitch, 0)
  if (kStageShadowSyncWithCamera && have_view) {
    const float th = float(std::atan2(double(-v22), double(-v20)));   // c = (0, 0, -1) x V: (c.x, c.z) = (-V[2][0], -V[2][2])
    const float co = std::cos(th), si = std::sin(th);
    const float x = d[0] * co + d[2] * si, z = -d[0] * si + d[2] * co;
    d[0] = x, d[2] = z;
  }
  const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  out[0] = d[0] / n, out[1] = d[1] / n, out[2] = d[2] / n;
}

}  // namespace rdr_scene
