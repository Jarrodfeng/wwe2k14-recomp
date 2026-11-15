// S102 lane L1 (docs/renderer/PRELIGHT_DIRECT.md section 8): the pre-light cube evaluated per pixel instead of sampled.
// Our own code. Included by fx_skin.glsl, fx_gear.glsl and fx_trans_ps.glsl AFTER their FragmentConstants block (instance `fc`), only under FX_PRELIGHT_ANALYTIC.
//
// The game bakes the lights into a 64x64x6 RGBA8 cube every frame (the yPreLight 'sss' program: yPreLight.frag, FX_SSS) and the character shaders sample it at the surface normal. The cube is a function of
// four directional lights, so the same function is evaluated here at the normal: no cube, no bake passes, no 64x64 quantisation. The block member PreLightK holds that bake's constants, PRECOMPUTED on the CPU
// (prelight_analytic.h Precompute) so that only what depends on the normal is left per pixel:
//   [0..2] the four light directions transposed: x, y, z of lights 0..3            (d = n.x K0 + n.y K1 + n.z K2: the four N.L values in three MADs)
//   [3]    the colour luma of each light                                          [4] that times DifLghMaxA.y
//   [5]    (1 - DifLghPrm.x) / 2 of each light (how much of its weight is NOT pulled to the shared mean, halved)
//   [6..8] the light colours transposed: r, g, b of lights 0..3, times DifLghMaxD.xyz
//   [9..11]  lobe A: (4 tune.x, pivot, ceiling, offset) | (shape.y, -shape.z) | the three-knot curve as a quadratic polynomial (k.x - 0.5, 2 (k.y - k.x), k.x - 2 k.y + k.z)
//   [12..14] lobe B: the same
//   [15..16] unused    [17].x TEST ONLY: 1 = round the result to 8 bits as the cube stored it (the offline proof's quantise arm); the binder writes 0
// `n` must be a unit vector (all three call sites pass fx_normalize'd normals). The result is what the bake would have stored: rgb = the lit colour times MaxD, a = the scattering term, both clamped to
// 0..1 as the RGBA8 target clamps them.
#ifndef FX_PRELIGHT_GLSL
#define FX_PRELIGHT_GLSL

#ifdef FX_PRELIGHT_ANALYTIC

// One lobe: the signed response of the four lights, 0 at the pivot (the curve is 0.5 there, continuous).
vec4 fx_pl_lobe(vec4 d, vec4 a, vec4 b, vec4 c) {
  vec4 folded = a.z - clamp(abs(d - a.y) + a.y, 0.0, 1.0);
  vec4 t = (sqrt(abs(b.x - a.x * folded)) + a.w) * b.y;
  return sign(d - a.y) * (c.x + t * (c.y + t * c.z));
}

vec4 fx_prelight_eval(vec3 n) {
  vec4 d = n.x * fc.PreLightK[0] + n.y * fc.PreLightK[1] + n.z * fc.PreLightK[2];
  vec4 wA = 0.5 + fx_pl_lobe(d, fc.PreLightK[9], fc.PreLightK[10], fc.PreLightK[11]);
  vec4 wB = 0.5 + fx_pl_lobe(d, fc.PreLightK[12], fc.PreLightK[13], fc.PreLightK[14]);
  vec4 weighted = wA * fc.PreLightK[3];
  float weight_sum = weighted.x + weighted.y + weighted.z + weighted.w;
  // A surface turned away from all four lights has weight_sum 0: the bake stored NaN there, which its 8-bit target stores as 0.
  if (!(weight_sum > 1e-6)) return vec4(0.0);
  float dominance = max(max(weighted.x, weighted.y), max(weighted.z, weighted.w)) / weight_sum;
  // mix(wB, mean, pull) = wB + h * (wA - wB) and mix(wA, mean, pull) = wA - h * (wA - wB), with h = pull / 2 = 0.5 - dominance * K5
  vec4 diff = wA - wB;
  vec4 h = 0.5 - dominance * fc.PreLightK[5];
  vec4 wColor = wB + h * diff;
  vec4 wAlpha = wA - h * diff;
  vec4 result = clamp(vec4(dot(wColor, fc.PreLightK[6]), dot(wColor, fc.PreLightK[7]), dot(wColor, fc.PreLightK[8]), dot(wAlpha, fc.PreLightK[4])), 0.0, 1.0);
  if (fc.PreLightK[17].x > 0.5) result = floor(result * 255.0 + 0.5) / 255.0;   // TEST ONLY: what the RGBA8 cube stored
  return result;
}

#endif  // FX_PRELIGHT_ANALYTIC
#endif  // FX_PRELIGHT_GLSL
