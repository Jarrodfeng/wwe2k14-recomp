#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the entrance's star glare directional blur (the
// game's yBlur, guest pixel shader 75641266549A40C0; docs\systems\post_effects.md, "Entrance": eight passes at 160x128),
// through the shader swap (src\scene_swap.h): the source read at the pixel's coordinate moved by eight offsets (c0..c7,
// x and y), every tap's four channels multiplied by its own weight vector (c16..c23) and added up. The guest's
// multiply-adds run with the accumulator's channels permuted from step to step (the swizzles are kept exactly as
// written, so the permutation ends in the right order). The fetch constant's LOD bias and exponent adjust are kept as
// the guest applies them (the draw's own fetch constants, set 1 binding 4). Float constants (set 1 binding 2): 0, 1, 2,
// 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[16]; } fc;
#define C0  fc.c[0]   // xy: offsets of the taps (c0..c7)
#define C1  fc.c[1]
#define C2  fc.c[2]
#define C3  fc.c[3]
#define C4  fc.c[4]
#define C5  fc.c[5]
#define C6  fc.c[6]
#define C7  fc.c[7]
#define W16 fc.c[8]   // per-channel weights of the taps (c16..c23)
#define W17 fc.c[9]
#define W18 fc.c[10]
#define W19 fc.c[11]
#define W20 fc.c[12]
#define W21 fc.c[13]
#define W22 fc.c[14]
#define W23 fc.c[15]

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
  vec4 t7 = Tap(C7.xy);  // each tap is named by its offset register
  vec4 t2 = Tap(C2.xy);
  vec4 t0 = Tap(C0.xy);
  vec4 t5 = Tap(C5.xy);
  vec4 t3 = Tap(C3.xy);
  vec4 t4 = Tap(C4.xy);
  vec4 t1 = Tap(C1.xy);
  vec4 t6 = Tap(C6.xy);
  vec4 acc = t6 * W22;
  acc = fma(t5, W21, acc);
  acc = fma(t4.xzwy, W20.xzwy, acc.xzwy);
  acc = fma(t3.xwyz, W19.xwyz, acc.xzwy);
  acc = fma(t2, W18, acc.xzwy);
  acc = fma(t0.xzwy, W16.xzwy, acc.xzwy);
  acc = fma(t1.xwyz, W17.xwyz, acc.xzwy);
  XeOutputColor(fma(t7, W23, acc.xzwy));
}
