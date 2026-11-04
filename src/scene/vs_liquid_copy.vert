#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the wrestlers' sweat map copy, the game's yLiquidMap
// (guest vertex shader 5D2AE398B797CF87, a vertex-declaration copy of base shader 1C56621A...; 0 / 6 / 6 draws a frame in
// Brock's entrance / Bryan's entrance / the match). Pixel side: liquid_map_copy.frag (guest pixel shader F235532B75570EAF).
// Written from our reading of the guest microcode; checked against the translation by tools\shaders\harness\vs_harness.py.
//
// 6-dword vertices: a 2-float position (dwords 0-1) and 2 floats at dwords 4-5. The position is mirrored about the middle of c206
// ((c206.xy - p x c206.zw) ... as the guest writes it) and put in clip space by c201 (xy scale, zw offset); z = 0, w = 1. The
// second interpolator carries the fade terms (the 360's power curve from log2 / exp2 and the reciprocals of c202.xy).
// Interpolators: 0 = (0, 0, mapped x, mapped y); 1 = (p.y^2 x |p.y|, 1 - 2^(...), the two floats).
// Float constants (set 1 binding 1, packed in register order): 201 202 206 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[4]; } fc;
#define C201  fc.c[0]
#define C202  fc.c[1]
#define C206  fc.c[2]
#define C255  fc.c[3]
#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_a;
layout(location = 1) invariant out vec4 out_b;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 6, 95);
  vec4 r0 = vec4(0.0), r1 = vec4(0.0), r2, r3 = vec4(0.0);
  r0.y = XeFetchFloat(at);
  r0.w = XeFetchFloat(at + 1);
  r1.zw = vec2(XeFetchFloat(at + 4), XeFetchFloat(at + 5));
  precise vec4 scaled = vec4(r0.y, r0.w, r0.y, r0.w) * C206;
  r2 = scaled;
  r3.x = 1.0 / C202.x;
  precise vec2 mirrored = (-r2.xy) + C206.xy;
  r0.x = mirrored.x;
  r0.z = mirrored.y;
  precise vec2 sum = r0.xz + r2.zw;
  r2.xy = sum;
  vec2 xy = AvFma(r2.xy, C201.xy, C201.zw);
  XeOutputPosition(vec4(xy, 0.0, 1.0));
  precise float shifted = (-r1.w) + C255.z;
  r0.x = shifted;
  r3.y = 1.0 / C202.y;
  precise float squared = abs(r0.w) * abs(r0.w);
  r1.x = squared;
  r3.z = log2(abs(r0.x));
  r0.xyz = vec3(AvMulX(r3.x, C255.x), AvMulX(r3.y, C255.x), AvMulX(r3.z, C255.y));
  precise vec2 moved = r2.xy + r0.xy;
  out_a = vec4(0.0, 0.0, moved);
  precise float cubed = r1.x * abs(r0.w);
  r1.x = cubed;
  r0.x = exp2(r0.z);
  precise float rest = C255.z - r0.x;
  r1.y = rest;
  out_b = r1;
}
