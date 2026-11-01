#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): a four-tap mask grow (the game's yBlur, guest
// pixel shader 0DC910FA641514C0; the first of the two mask blurs), through the shader swap (src\scene_swap.h): the
// source's red read at the pixel's coordinate moved by four offsets, each tap turned into 1 when it differs from
// c255.x (else 0), and the flags folded one by one, each partial sum tested against c255.x again (with c255.x = 0 this
// is "any tap set"); the result in red, green and blue, alpha 1. The fetch constant's LOD bias and exponent adjust are
// kept as the guest applies them (the draw's own fetch constants, set 1 binding 4). Float constants (set 1 binding 2):
// 0, 1, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[3]; } fc;
#define C0   fc.c[0]  // xy, zw: offsets of the fourth and third taps
#define C1   fc.c[1]  // xy, zw: offsets of the second and first taps
#define C255 fc.c[2]  // x: the value a tap must differ from

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;
#define FETCH0_WORD int(xe_fetch.fetch_constants[1].x)
float LodBias(int word) { return float(bitfieldExtract(word, 12, 10)) * 0.03125; }
float ExponentScale(int word) { return ldexp(1.0, bitfieldExtract(word, 13, 6)); }

layout(set = 3, binding = 1000) uniform texture2DArray t_source;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_source;

layout(location = 0) in vec4 in_uv;  // xy: the pixel's coordinate in the source

float Tap(vec2 offset) {
  int word = FETCH0_WORD;
  vec3 at = vec3(in_uv.xy + offset, 0.0);
  return texture(sampler2DArray(t_source, s_source), at, LodBias(word)).x * ExponentScale(word);
}
float Flag(float v) { return v != C255.x ? 1.0 : 0.0; }

void main() {
  float t0 = Tap(C1.zw);
  float t1 = Tap(C1.xy);
  float t2 = Tap(C0.zw);
  float t3 = Tap(C0.xy);
  float s1 = Flag(t3) + Flag(t2);
  float s2 = Flag(s1) + Flag(t1);
  float s3 = Flag(s2) + Flag(t0);
  float v = Flag(s3);
  XeOutputColor(vec4(v, v, v, 1.0));
}
