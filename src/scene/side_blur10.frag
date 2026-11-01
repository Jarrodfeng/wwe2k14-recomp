#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): a ten-tap blur of the shadow maps (the game's
// yBlur, guest pixel shader 7C650AA1C792DFF6; two source images, red channel only, five taps each), through the shader
// swap (src\scene_swap.h): image q (fetch constant 0) at four offsets (c0 xy and zw, c1 xy and zw) and the pixel's own
// coordinate, image p (fetch constant 1) at four offsets (c2 xy and zw, c3 xy and zw) and the pixel's coordinate. m =
// the larger of q and p tap by tap. Red = the c16-weighted sum of q's four offset taps plus c17.x times its centre tap,
// blue = the same sum over m (its centre being the larger of the two centre taps); green = the smallest of q's five
// taps, alpha = the smallest of m's five. The fetch constants' LOD bias and exponent adjust are kept as the guest
// applies them (the draw's own fetch constants, set 1 binding 4). Float constants (set 1 binding 2): 0, 1, 2, 3, 16,
// 17.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[6]; } fc;
#define C0  fc.c[0]  // image q: xy, zw: offsets of the first two taps
#define C1  fc.c[1]  // image q: xy, zw: offsets of the next two taps
#define C2  fc.c[2]  // image p: xy, zw: offsets of the first two taps
#define C3  fc.c[3]  // image p: xy, zw: offsets of the next two taps
#define C16 fc.c[4]  // the four offset taps' weights
#define C17 fc.c[5]  // x: the centre tap's weight

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;
#define FETCH0_WORD int(xe_fetch.fetch_constants[1].x)
#define FETCH1_WORD int(xe_fetch.fetch_constants[2].z)
float LodBias(int word) { return float(bitfieldExtract(word, 12, 10)) * 0.03125; }
float ExponentScale(int word) { return ldexp(1.0, bitfieldExtract(word, 13, 6)); }

layout(set = 3, binding = 1000) uniform texture2DArray t_q;  // fetch constant 0
layout(set = 3, binding = 1001) uniform texture2DArray t_p;  // fetch constant 1
layout(set = 3, binding = 1200) uniform sampler s_q;
layout(set = 3, binding = 1201) uniform sampler s_p;

layout(location = 0) in vec4 in_uv;  // xy: the pixel's coordinate in both sources

float TapQ(vec2 offset) {
  int word = FETCH0_WORD;
  return texture(sampler2DArray(t_q, s_q), vec3(in_uv.xy + offset, 0.0), LodBias(word)).x * ExponentScale(word);
}
float TapP(vec2 offset) {
  int word = FETCH1_WORD;
  return texture(sampler2DArray(t_p, s_p), vec3(in_uv.xy + offset, 0.0), LodBias(word)).x * ExponentScale(word);
}

// The weighted sum, the last tap's product first and the first tap's added last.
float Weighted(vec4 t) {
  float s = t.w * C16.w;
  s = fma(t.z, C16.z, s);
  s = fma(t.x, C16.x, s);
  return fma(t.y, C16.y, s);
}

void main() {
  vec4 q = vec4(TapQ(C0.xy), TapQ(C0.zw), TapQ(C1.xy), TapQ(C1.zw));
  vec4 p = vec4(TapP(C2.xy), TapP(C2.zw), TapP(C3.xy), TapP(C3.zw));
  float q_centre = TapQ(vec2(0.0));
  float p_centre = TapP(vec2(0.0));
  vec4 m = max(q, p);
  float m_centre = max(q_centre, p_centre);
  float red = fma(q_centre, C17.x, Weighted(q));
  float blue = fma(m_centre, C17.x, Weighted(m));
  float green = min(min(min(q.x, q.y), min(q.z, q.w)), q_centre);
  float alpha = min(min(min(m.x, m.y), min(m.z, m.w)), m_centre);
  XeOutputColor(vec4(red, green, blue, alpha));
}
