#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the sweat map's layer pass (the game's
// yLiquidMap, guest pixel shader 022F374B74F15579; the draws that lay the liquid's shape over a base colour), through
// the shader swap (src\scene_swap.h). The second texture is scaled by c203 and its largest channel m taken. A fade
// e = (c255.x - a * b) raised to the power c255.y (a, b: the vertex shader's first two terms) turns m into
// min(m * (c255.x - e), c255.x), and that is blended from m towards itself by c204.z. Red and green = m times
// c204.x / c204.y, blue = the blend times c204.z, each added to the first texture (read at an explicit level, the
// guest's fixed LOD) times (c255.x - c204.xyz); alpha = m times the vertex shader's fourth term. The console's
// 0 x infinity = 0 is kept in the power and the products. The fetch constants' LOD bias and exponent adjust are kept
// as the guest applies them (the draw's own fetch constants, set 1 binding 4): the pass reads a chosen mip level of the
// first texture, the level its fetch constant's bias field selects. Float constants (set 1 binding 2): 203, 204, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[3]; } fc;
#define C203 fc.c[0]  // the second texture's scale per channel
#define C204 fc.c[1]  // xyz: the layer's colour weights, z also the blend
#define C255 fc.c[2]  // x: one (the compiler's literal bank), y: power of the fade

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;
#define FETCH0_WORD int(xe_fetch.fetch_constants[1].x)
#define FETCH1_WORD int(xe_fetch.fetch_constants[2].z)
float LodBias(int word) { return float(bitfieldExtract(word, 12, 10)) * 0.03125; }
float ExponentScale(int word) { return ldexp(1.0, bitfieldExtract(word, 13, 6)); }

layout(set = 3, binding = 1000) uniform texture2DArray t_base;   // fetch constant 0
layout(set = 3, binding = 1001) uniform texture2DArray t_shape;  // fetch constant 1
layout(set = 3, binding = 1200) uniform sampler s_base;
layout(set = 3, binding = 1201) uniform sampler s_shape;

layout(location = 0) in vec4 in_uv;      // xy: the shape's coordinate, zw: the base's coordinate
layout(location = 1) in vec4 in_params;  // x, y: the fade's two terms, w: alpha scale

void main() {
  int base_word = FETCH0_WORD, shape_word = FETCH1_WORD;
  vec3 base_at = vec3(in_uv.zw, 0.0), shape_at = vec3(in_uv.xy, 0.0);
  vec3 base = textureLod(sampler2DArray(t_base, s_base), base_at, LodBias(base_word)).xyz * ExponentScale(base_word);
  vec4 shape = texture(sampler2DArray(t_shape, s_shape), shape_at, LodBias(shape_word)) * ExponentScale(shape_word);
  shape *= C203;
  float fade_base = fma(-in_params.y, in_params.x, C255.x);
  float m = max(max(max(shape.x, shape.y), shape.z), shape.w);
  float e = PowAbs(fade_base, C255.y);
  float u = Mul0(C255.x - e, m);
  u = u < C255.x ? u : C255.x;
  float v = u - m;
  float w = Mul0(v, C204.z) + m;
  vec3 layer = vec3(Mul0(m, C204.x), Mul0(m, C204.y), Mul0(w, C204.z));
  vec3 rgb = vec3(Mul0(C255.x - C204.x, base.x), Mul0(C255.x - C204.y, base.y), Mul0(C255.x - C204.z, base.z)) + layer;
  XeOutputColor(vec4(rgb, m * in_params.w));
}
