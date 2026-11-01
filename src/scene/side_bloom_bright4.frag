#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the entrance bloom's bright pass (the game's
// yDownScale, guest pixel shader 156FA9A105B45F9B), through the shader swap (src\scene_swap.h): a four-tap downscale as
// side_downscale4.frag (the source read at the pixel's coordinate moved by each of four offsets c0..c3, the colours
// added second offset first, then the first, third and fourth, alpha included) with the sum scaled by c255.x and the
// threshold c16.x taken off the three colour channels (one fused multiply-add); alpha is the sum's times c255.x only.
// The fetch constant's LOD bias and exponent adjust are kept as the guest applies them (the draw's own fetch
// constants, set 1 binding 4). Float constants (set 1 binding 2): 0, 1, 2, 3, 16, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[6]; } fc;
#define C0   fc.c[0]  // xy: offset of the first tap
#define C1   fc.c[1]  // xy: offset of the second tap
#define C2   fc.c[2]  // xy: offset of the third tap
#define C3   fc.c[3]  // xy: offset of the fourth tap
#define C16  fc.c[4]  // x: the bright pass's threshold
#define C255 fc.c[5]  // x: scale of the sum

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
  vec4 a = Tap(C0.xy);
  vec4 b = Tap(C1.xy);
  vec4 c = Tap(C2.xy);
  vec4 d = Tap(C3.xy);
  vec4 sum = ((b + a) + c) + d;
  XeOutputColor(vec4(fma(sum.xyz, C255.xxx, -C16.xxx), sum.w * C255.x));
}
