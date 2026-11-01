#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the entrance's star glare streak combine (the
// game's yBlur, guest pixel shader 9C84E33D39058FB0; docs\systems\post_effects.md, "Entrance"), through the shader
// swap (src\scene_swap.h): four streak images read at the pixel's coordinate (fetch constants 0..3) and added up, each
// times its own weight vector (c16 on the first, c17 on the second, c18 on the third, c19 on the fourth). The guest
// accumulates from the fourth image down, and its last two multiply-adds permute channels: the second image's step
// takes its operands in the order x, z, w, y (so the result's y holds the z channels' product-sum, its z the w
// channels', its w the y channels'), and the first image's step then reads that sum in the order x, w, y, z, which
// puts every channel back in place. Those swizzles are kept exactly as written. The fetch constants' LOD bias and
// exponent adjust are kept as the guest applies them (the draw's own fetch constants, set 1 binding 4). Float
// constants (set 1 binding 2): 16, 17, 18, 19.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[4]; } fc;
#define C16 fc.c[0]  // weights of the first streak image
#define C17 fc.c[1]  // weights of the second
#define C18 fc.c[2]  // weights of the third
#define C19 fc.c[3]  // weights of the fourth

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;
#define FETCH0_WORD int(xe_fetch.fetch_constants[1].x)
#define FETCH1_WORD int(xe_fetch.fetch_constants[2].z)
#define FETCH2_WORD int(xe_fetch.fetch_constants[4].x)
#define FETCH3_WORD int(xe_fetch.fetch_constants[5].z)
float LodBias(int word) { return float(bitfieldExtract(word, 12, 10)) * 0.03125; }
float ExponentScale(int word) { return ldexp(1.0, bitfieldExtract(word, 13, 6)); }

layout(set = 3, binding = 1000) uniform texture2DArray t_streak0;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_streak0;
layout(set = 3, binding = 1001) uniform texture2DArray t_streak1;  // fetch constant 1
layout(set = 3, binding = 1201) uniform sampler s_streak1;
layout(set = 3, binding = 1002) uniform texture2DArray t_streak2;  // fetch constant 2
layout(set = 3, binding = 1202) uniform sampler s_streak2;
layout(set = 3, binding = 1003) uniform texture2DArray t_streak3;  // fetch constant 3
layout(set = 3, binding = 1203) uniform sampler s_streak3;

layout(location = 0) in vec4 in_uv;  // xy: the pixel's coordinate in the streak images

void main() {
  int word0 = FETCH0_WORD;
  int word1 = FETCH1_WORD;
  int word2 = FETCH2_WORD;
  int word3 = FETCH3_WORD;
  vec4 s0 = texture(sampler2DArray(t_streak0, s_streak0), vec3(in_uv.xy, 0.0), LodBias(word0)) * ExponentScale(word0);
  vec4 s1 = texture(sampler2DArray(t_streak1, s_streak1), vec3(in_uv.xy, 0.0), LodBias(word1)) * ExponentScale(word1);
  vec4 s2 = texture(sampler2DArray(t_streak2, s_streak2), vec3(in_uv.xy, 0.0), LodBias(word2)) * ExponentScale(word2);
  vec4 s3 = texture(sampler2DArray(t_streak3, s_streak3), vec3(in_uv.xy, 0.0), LodBias(word3)) * ExponentScale(word3);
  vec4 sum = s3 * C19;
  sum = fma(s2, C18, sum);
  // the second image's pairs added crosswise: x with x, y from z, z from w, w from y
  vec4 mixed = fma(s1.xzwy, C17.xzwy, sum.xzwy);
  XeOutputColor(fma(s0, C16, mixed.xwyz));
}
