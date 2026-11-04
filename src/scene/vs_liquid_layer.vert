#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the wrestlers' sweat map layer, the game's
// yLiquidMap (guest vertex shader 8F60149686CB59D1, a vertex-declaration copy of base shader 394C71AB...; 0 / 2 / 2 draws a
// frame in Brock's entrance / Bryan's entrance / the match). Pixel side: side_liquid_layer.frag (guest pixel shader
// 022F374B74F15579). Written from our reading of the guest microcode; checked against the translation by
// tools\shaders\harness\vs_harness.py.
//
// As vs_liquid_copy.vert, plus the mapped coordinate of the layer's source rectangle: ((position - c205.xy) times the
// reciprocal of (c205.zw - c205.xy)) in the first interpolator's xy. Interpolators: 0 = (that, then the mapped x, y of the
// copy); 1 = the fade terms as there.
// Float constants (set 1 binding 1, packed in register order): 201 202 205 206 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[5]; } fc;
#define C201  fc.c[0]
#define C202  fc.c[1]
#define C205  fc.c[2]
#define C206  fc.c[3]
#define C255  fc.c[4]
#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_a;
layout(location = 1) invariant out vec4 out_b;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 6, 95);
  vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2, r3 = vec4(0.0), r4 = vec4(0.0);
  r0.y = XeFetchFloat(at);
  r0.w = XeFetchFloat(at + 1);
  r1.zw = vec2(XeFetchFloat(at + 4), XeFetchFloat(at + 5));
  precise vec4 scaled = vec4(r0.y, r0.w, r0.y, r0.w) * C206;
  r2 = scaled;
  precise vec2 mirrored = (-r2.xy) + C206.xy;
  r0.x = mirrored.x;
  r0.z = mirrored.y;
  precise vec2 sum = r0.xz + r2.zw;
  r2.xy = sum;
  vec2 xy = AvFma(r2.xy, C201.xy, C201.zw);
  XeOutputPosition(vec4(xy, 0.0, 1.0));
  precise vec2 span = C205.zw + (-C205.xy);
  r2.zw = span;
  r4.x = 1.0 / C202.x;
  precise float squared = abs(r0.w) * abs(r0.w);
  r1.x = squared;
  r4.y = 1.0 / C202.y;
  precise float shifted = (-r1.w) + C255.z;
  r0.x = shifted;
  r2.z = 1.0 / r2.z;
  precise vec2 offset = r2.xy + (-C205.xy);
  r3.xy = offset;
  r4.z = log2(abs(r0.x));
  r0.xyz = vec3(AvMulX(r4.x, C255.x), AvMulX(r4.y, C255.x), AvMulX(r4.z, C255.y));
  r2.w = 1.0 / r2.w;
  vec2 mapped = AvMulX(r3.xy, r2.zw);
  precise vec2 moved = r2.xy + r0.xy;
  out_a = vec4(mapped, moved);
  precise float cubed = r1.x * abs(r0.w);
  r1.x = cubed;
  r0.x = exp2(r0.z);
  precise float rest = C255.z - r0.x;
  r1.y = rest;
  out_b = r1;
}
