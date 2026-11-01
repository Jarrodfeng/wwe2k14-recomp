#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the match's depth-aware downsample for the depth
// of field (the game's yDownScale, guest pixel shader A7AD68BE21C46E7A; docs\systems\post_effects.md, "Match" pass 1),
// through the shader swap (src\scene_swap.h). Four taps of the scene colour at the pixel's coordinate moved by c0, c1,
// c2 and c3, each paired with the depth read at the same offset (c1, c2, c3; the c0 tap has none), and the pixel's own
// depth. With z = c255.x minus a depth and limit = (c255.x - c21.w) - own z, a tap counts when (c255.x minus its depth)
// is at least limit. The first colour is the c0 tap; the c1 tap is added to it only when it counts (else the sum is
// the c0 tap alone, and the count is c255.x, else c255.y); the c2 and c3 taps are then read and added only when they
// count, each adding c255.x to the count. The colour out is the sum times 1 / count (the console's 0 x infinity = 0
// kept); alpha is 1 when the pixel's own z lies between c21.x and c21.y, else 0 (the blur pass side_dof_blur.frag
// skips the pixels with alpha above 0, so the band is the in-focus one: unconfirmed).
// The fetch constants' LOD bias and exponent adjust are kept as the guest applies them (the draw's own fetch
// constants, set 1 binding 4); the depth texture is a float image whose red is the depth. Float constants (set 1
// binding 2): 0, 1, 2, 3, 21, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[6]; } fc;
#define C0   fc.c[0]  // xy: offset of the first tap
#define C1   fc.c[1]  // xy: offset of the second tap
#define C2   fc.c[2]  // xy: offset of the third tap
#define C3   fc.c[3]  // xy: offset of the fourth tap
#define C21  fc.c[4]  // x, y: the blur range's limits in z; w: the margin below the pixel's own z
#define C255 fc.c[5]  // x: one; y: two (the compiler's literal bank)

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;
#define FETCH0_WORD int(xe_fetch.fetch_constants[1].x)
#define FETCH1_WORD int(xe_fetch.fetch_constants[2].z)
float LodBias(int word) { return float(bitfieldExtract(word, 12, 10)) * 0.03125; }
float ExponentScale(int word) { return ldexp(1.0, bitfieldExtract(word, 13, 6)); }

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0: the scene colour
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1001) uniform texture2DArray t_depth;  // fetch constant 1: the depth
layout(set = 3, binding = 1201) uniform sampler s_depth;

layout(location = 0) in vec4 in_uv;  // xy: the pixel's coordinate in the source

vec3 Color(vec2 at) {
  int word = FETCH0_WORD;
  return (texture(sampler2DArray(t_color, s_color), vec3(at, 0.0), LodBias(word)) * ExponentScale(word)).rgb;
}

float Depth(vec2 at) {
  int word = FETCH1_WORD;
  return texture(sampler2DArray(t_depth, s_depth), vec3(at, 0.0), LodBias(word)).x * ExponentScale(word);
}

void main() {
  vec2 at0 = in_uv.xy + C0.xy;
  vec2 at2 = in_uv.xy + C2.xy;
  vec2 at1 = in_uv.xy + C1.xy;
  float depth1 = Depth(at1);
  float depth2 = Depth(at2);
  vec3 color1 = Color(at1);
  vec3 color0 = Color(at0);
  float own = Depth(in_uv.xy);
  float margin = C255.x - C21.w;
  vec3 sum = color1 + color0;
  float limit = margin - own;
  bool use1 = (C255.x - depth1) >= limit;
  bool use2 = (C255.x - depth2) >= limit;
  if (!use1) sum = color0;
  float ownz = C255.x - own;
  float count = use1 ? C255.y : C255.x;
  if (use2) {
    sum = Color(at2) + sum;
    count = C255.x + count;
  }
  vec2 at3 = in_uv.xy + C3.xy;
  float z3 = C255.x - Depth(at3);
  if (z3 >= limit) {
    sum = Color(at3) + sum;
    count = C255.x + count;
  }
  float in_range = float(ownz >= C21.x) * float(C21.y >= ownz);
  float inv_count = 1.0 / count;
  XeOutputColor(vec4(Mul0(inv_count, sum.x), Mul0(inv_count, sum.y), Mul0(inv_count, sum.z), float(in_range != 0.0)));
}
