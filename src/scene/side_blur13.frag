#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): a thirteen-tap weighted blur (the game's yBlur,
// guest pixel shader 632189BAFAE233F0; the rim light map's blur and the entrance bloom's), through the shader swap
// (src\scene_swap.h): the source read at the pixel's coordinate moved by thirteen offsets (c0..c12, x and y), every
// tap's four channels multiplied by its own weight vector (c16..c28) and added up. The guest's multiply-adds run with
// the accumulator's channels permuted from step to step (the swizzles are kept exactly as written, so the permutation
// ends in the right order). The fetch constant's LOD bias and exponent adjust are kept as the guest applies them (the
// draw's own fetch constants, set 1 binding 4). Float constants (set 1 binding 2): 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
// 11, 12, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[26]; } fc;
#define C0  fc.c[0]   // xy: offsets of the taps (c0..c12)
#define C1  fc.c[1]
#define C2  fc.c[2]
#define C3  fc.c[3]
#define C4  fc.c[4]
#define C5  fc.c[5]
#define C6  fc.c[6]
#define C7  fc.c[7]
#define C8  fc.c[8]
#define C9  fc.c[9]
#define C10 fc.c[10]
#define C11 fc.c[11]
#define C12 fc.c[12]
#define W16 fc.c[13]  // per-channel weights of the taps (c16..c28)
#define W17 fc.c[14]
#define W18 fc.c[15]
#define W19 fc.c[16]
#define W20 fc.c[17]
#define W21 fc.c[18]
#define W22 fc.c[19]
#define W23 fc.c[20]
#define W24 fc.c[21]
#define W25 fc.c[22]
#define W26 fc.c[23]
#define W27 fc.c[24]
#define W28 fc.c[25]

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
  vec4 t12 = Tap(C12.xy);  // each tap is named by its offset register
  vec4 t10 = Tap(C10.xy);
  vec4 t9 = Tap(C9.xy);
  vec4 t8 = Tap(C8.xy);
  vec4 t7 = Tap(C7.xy);
  vec4 t6 = Tap(C6.xy);
  vec4 t5 = Tap(C5.xy);
  vec4 t4 = Tap(C4.xy);
  vec4 t3 = Tap(C3.xy);
  vec4 t2 = Tap(C2.xy);
  vec4 t0 = Tap(C0.xy);
  vec4 t1 = Tap(C1.xy);
  vec4 t11 = Tap(C11.xy);
  vec4 acc = t12 * W28;
  acc = fma(t10, W26, acc);
  acc = fma(t9.xwyz, W25.xwyz, acc.xwyz);
  acc = fma(t8, W24, acc.xzwy);
  acc = fma(t7.xzwy, W23.xzwy, acc.xzwy);
  acc = fma(t6.xwyz, W22.xwyz, acc.xzwy);
  acc = fma(t5, W21, acc.xzwy);
  acc = fma(t4.xzwy, W20.xzwy, acc.xzwy);
  acc = fma(t3.xwyz, W19.xwyz, acc.xzwy);
  acc = fma(t2, W18, acc.xzwy);
  acc = fma(t0.xzwy, W16.xzwy, acc.xzwy);
  acc = fma(t1.xwyz, W17.xwyz, acc.xzwy);
  XeOutputColor(fma(t11, W27, acc.xzwy));
}
