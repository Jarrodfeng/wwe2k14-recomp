#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): a five-tap blur of the shadow maps (the game's
// yBlur, guest pixel shader 694A6D148DC33CFF; red and blue are blurred, green and alpha keep the smallest), through
// the shader swap (src\scene_swap.h): the source read at the pixel's coordinate and at four offsets (c0 xy and zw, c1
// xy and zw). Red and blue = the weighted sum of the taps (weights c16.x on the c0 xy tap, c16.y on the c0 zw tap,
// c16.z on the c1 xy tap, c16.w on the c1 zw tap, c17.x on the centre tap); green and alpha = the smallest of the five
// taps' green and alpha. The fetch constant's LOD bias and exponent adjust are kept as the guest
// applies them (the draw's own fetch constants, set 1 binding 4). Float constants (set 1 binding 2): 0, 1, 16, 17.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[4]; } fc;
#define C0  fc.c[0]  // xy, zw: offsets of the taps weighted by c16.x and c16.y
#define C1  fc.c[1]  // xy, zw: offsets of the taps weighted by c16.z and c16.w
#define C16 fc.c[2]  // the four offset taps' weights
#define C17 fc.c[3]  // x: the centre tap's weight

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;
#define FETCH0_WORD int(xe_fetch.fetch_constants[1].x)
float LodBias(int word) { return float(bitfieldExtract(word, 12, 10)) * 0.03125; }
float ExponentScale(int word) { return ldexp(1.0, bitfieldExtract(word, 13, 6)); }

layout(set = 3, binding = 1000) uniform texture2DArray t_source;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_source;

layout(location = 0) in vec4 in_uv;  // xy: the pixel's coordinate in the source

vec4 Tap(vec2 offset) {
  int word = FETCH0_WORD;
  return texture(sampler2DArray(t_source, s_source), vec3(in_uv.xy + offset, 0.0), LodBias(word)) * ExponentScale(word);
}

void main() {
  vec4 a = Tap(C1.xy);
  vec4 b = Tap(C1.zw);
  vec4 c = Tap(C0.xy);
  vec4 d = Tap(C0.zw);
  vec4 e = Tap(vec2(0.0));
  // red and blue: the weighted sum, the centre tap first
  vec2 sum = e.xz * C17.x;
  sum = fma(b.xz, C16.ww, sum);
  sum = fma(a.xz, C16.zz, sum);
  sum = fma(d.xz, C16.yy, sum);
  sum = fma(c.xz, C16.xx, sum);
  // green and alpha: the smallest tap
  vec2 low = min(min(c.yw, d.yw), min(a.yw, b.yw));
  low = min(low, e.yw);
  XeOutputColor(vec4(sum.x, low.x, sum.y, low.y));
}
