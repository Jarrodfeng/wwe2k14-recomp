#version 450
// Pre-computed light cube map, pixel stage. See docs\renderer\effects\yPreLight.md.
// Every pixel is a direction (a surface normal); the output is what the four directional lights do to a surface facing it.
// Options: FX_LIGHT_COLOR (sum of the four lights' colours weighted by N.L), FX_LIGHT_DOT (the four N.L values themselves),
//          FX_SPEC_POW (the four lights' specular power curves, one power per channel), FX_SOFT (soft-terminator version that also writes a scattering term in alpha),
//          FX_DEBUG_CUBE / FX_DEBUG_CUBE_ALPHA (shows a cube map; the alpha version as grey).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#if defined(FX_LIGHT_COLOR) || defined(FX_LIGHT_DOT) || defined(FX_SPEC_POW) || defined(FX_SOFT) || defined(FX_SSS)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 DifLghVec[4];
#if !defined(FX_LIGHT_DOT)
  vec4 DifLghCol[4];
  vec4 DifLghMaxD;       // xyz colour scale, w scale
#endif
#ifdef FX_SPEC_POW
  vec4 SpecPow;
#endif
#ifdef FX_SSS
  vec4 DifLghPrm[4];        // x of each: how far the light's weight is pulled to the shared mean
  vec4 DifLghMaxA;       // y scales the alpha
  vec4 SssLobeA[3];      // [0] curve-shape tuning, [1] three knots of the response curve, [2] bias and scale of the curve parameter
  vec4 SssLobeB[3];      // the same for the second lobe
#endif
#ifdef FX_SOFT
  vec4 SS;               // x gain, y and z the soft edge of N.L
#endif
} fc;
#endif

#if defined(FX_DEBUG_CUBE) || defined(FX_DEBUG_CUBE_ALPHA)
layout(set = 1, binding = 0) uniform samplerCube cubes[16];
#endif

#ifdef FX_SSS
// Parameter of the response curve: how far N.L is from the lobe's pivot, folded and shaped by the lobe's tuning (x gain, y offset, z ceiling, w pivot; shape.y bias, shape.z scale).
vec4 fx_sss_param(vec4 d, vec4 tune, vec4 shape) {
  vec4 folded = tune.z - clamp(abs(d - tune.w) + tune.w, 0.0, 1.0);
  return -(sqrt(abs(shape.y - 4.0 * tune.x * folded)) + tune.y) * shape.z;
}
// Quadratic curve through three knots (x, y, z), evaluated at t.
vec4 fx_sss_curve(vec4 t, vec4 knots) {
  return mix(mix(vec4(knots.x), vec4(knots.y), t), mix(vec4(knots.y), vec4(knots.z), t), t);
}
#endif

layout(location = 0) in vec4 inDirection;
layout(location = 0) out vec4 outColor;

void main() {
  vec3 n = fx_normalize(inDirection.xyz);
#if defined(FX_DEBUG_CUBE)
  outColor = vec4(texture(cubes[0], n).xyz, 1.0);
#elif defined(FX_DEBUG_CUBE_ALPHA)
  outColor = vec4(vec3(texture(cubes[0], n).w), 1.0);
#else
  vec4 d = vec4(dot(fc.DifLghVec[0].xyz, n), dot(fc.DifLghVec[1].xyz, n), dot(fc.DifLghVec[2].xyz, n), dot(fc.DifLghVec[3].xyz, n));
#if defined(FX_LIGHT_DOT)
  outColor = d;
#elif defined(FX_LIGHT_COLOR)
  vec4 lit = max(d, 0.0);
  outColor = lit.x * fc.DifLghCol[0] + lit.y * fc.DifLghCol[1] + lit.z * fc.DifLghCol[2] + lit.w * fc.DifLghCol[3];
#elif defined(FX_SPEC_POW)
  vec4 lit = max(d, 0.0);
  vec4 sum = vec4(0.0);
  for (int i = 0; i < 4; ++i) {
    vec4 curve = vec4(fx_pow(lit[i], fc.SpecPow.x), fx_pow(lit[i], fc.SpecPow.y), fx_pow(lit[i], fc.SpecPow.z), fx_pow(lit[i], fc.SpecPow.w));
    sum += curve * fc.DifLghCol[i].w;
  }
  outColor = sum * fc.DifLghMaxD.w;
#elif defined(FX_SSS)
  // Skin scattering. Per light, N.L is shaped into a parameter for a three-knot quadratic response curve (twice, one per lobe), and the lobes are combined.
  // The part of the weight that gets pulled to the shared mean grows with how much one light dominates.
  vec4 lum = vec4(dot(fc.DifLghCol[0].rgb, vec3(0.7553793787956238, 0.2366930991411209, 0.00792752020061016)), dot(fc.DifLghCol[1].rgb, vec3(0.7553793787956238, 0.2366930991411209, 0.00792752020061016)), dot(fc.DifLghCol[2].rgb, vec3(0.7553793787956238, 0.2366930991411209, 0.00792752020061016)), dot(fc.DifLghCol[3].rgb, vec3(0.7553793787956238, 0.2366930991411209, 0.00792752020061016)));
  vec4 pa = fx_sss_param(d, fc.SssLobeA[0], fc.SssLobeA[2]);
  vec4 pb = fx_sss_param(d, fc.SssLobeB[0], fc.SssLobeB[2]);
  vec4 lobeA = sign(d - fc.SssLobeA[0].w) * (fx_sss_curve(pa, fc.SssLobeA[1]) - 0.5);
  vec4 lobeB = sign(d - fc.SssLobeB[0].w) * (fx_sss_curve(pb, fc.SssLobeB[1]) - 0.5);
  vec4 wA = 0.5 + lobeA;
  vec4 wB = 0.5 + lobeB;
  vec4 mean = 0.5 * (wA + wB);
  vec4 weighted = wA * lum;
  float dominance = max(max(weighted.x, weighted.y), max(weighted.z, weighted.w)) / dot(weighted, vec4(1.0));
  vec4 pull = 1.0 - dominance * (1.0 - vec4(fc.DifLghPrm[0].x, fc.DifLghPrm[1].x, fc.DifLghPrm[2].x, fc.DifLghPrm[3].x));
  vec4 wColor = mix(wB, mean, pull);
  vec4 wAlpha = mix(wA, mean, pull);
  vec3 color = wColor.x * fc.DifLghCol[0].rgb + wColor.y * fc.DifLghCol[1].rgb + wColor.z * fc.DifLghCol[2].rgb + wColor.w * fc.DifLghCol[3].rgb;
  outColor = vec4(color * fc.DifLghMaxD.xyz, fc.DifLghMaxA.y * dot(wAlpha, lum));
#else
  vec4 lit = max(d, 0.0);
  vec3 color = lit.x * fc.DifLghCol[0].xyz + lit.y * fc.DifLghCol[1].xyz + lit.z * fc.DifLghCol[2].xyz + lit.w * fc.DifLghCol[3].xyz;
  // Each light's N.L goes through a smoothstep between SS.z and SS.y; the scattering term peaks in the soft part of each.
  vec4 t = clamp((lit - fc.SS.z) / (fc.SS.y - fc.SS.z), 0.0, 1.0);
  vec4 s = t * t * (3.0 - 2.0 * t);
  vec4 average = vec4(dot(fc.DifLghCol[0].xyz, vec3(1.0 / 3.0)), dot(fc.DifLghCol[1].xyz, vec3(1.0 / 3.0)), dot(fc.DifLghCol[2].xyz, vec3(1.0 / 3.0)), dot(fc.DifLghCol[3].xyz, vec3(1.0 / 3.0)));
  outColor = vec4(color * fc.DifLghMaxD.xyz, fc.SS.x * dot(s * s * (1.0 - s), average));
#endif
#endif
}
