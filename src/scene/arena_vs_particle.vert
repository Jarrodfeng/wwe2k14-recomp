#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena's particles, the game's yParticle (guest
// vertex shader C3570DF53E3AC981, a vertex-declaration copy of base shader 5F1EF76E...; 84 / 56 / 0 draws a frame in Brock's
// entrance / Bryan's entrance / the match). Pixel side: arena_particle.frag (guest pixel shader 34B29856453FE506). Written
// from our reading of the guest microcode; checked against the translation by tools\shaders\harness\vs_harness.py.
//
// 10-dword vertices: a position (3 floats), a colour (4 bytes), two texture coordinates (floats at dwords 6-7 and 8-9). The
// position goes through four matrix rows (c228..c231, then the view-projection rows c56..c59); a per-vertex fade of the
// colour comes from the vector to the camera (c59 less the position, normalised with the guest's reciprocal root) against
// the direction c230 - skipped when c255.y is not above c201.x (the draw's own switch, a compare on two constants).
// Interpolators: 0 = the second coordinate (floats at 6-7); 1 = the first (floats at 8-9); 2 = the colour x the fade.
// Float constants (set 1 binding 1, packed in register order): 56 57 58 59 200 201 202 228 229 230 231 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[12]; } fc;
#define C56   fc.c[0]
#define C57   fc.c[1]
#define C58   fc.c[2]
#define C59   fc.c[3]
#define C200  fc.c[4]
#define C201  fc.c[5]
#define C202  fc.c[6]
#define C228  fc.c[7]
#define C229  fc.c[8]
#define C230  fc.c[9]
#define C231  fc.c[10]
#define C255  fc.c[11]
#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_a;
layout(location = 1) invariant out vec4 out_b;
layout(location = 2) invariant out vec4 out_color;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 10, 95);
  vec4 r2 = vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), 1.0);
  vec4 r1 = AvFetchColorAt(at + 3);
  vec4 r3 = vec4(XeFetchFloat(at + 8), XeFetchFloat(at + 9), XeFetchFloat(at + 6), XeFetchFloat(at + 7));
  vec4 r0 = vec4(0.0), r4 = vec4(0.0), r5 = vec4(0.0), r6 = vec4(0.0);

  r0.x = (C255.y > C201.x) ? 1.0 : 0.0;
  r0.y = AvDot4(C202.wzxy, C202.wzxy);
  r6.x = AvDot4(C228.zxyw, r2.zxyw);
  r6.y = AvDot4(C229.zxyw, r2.zxyw);
  r6.z = AvDot4(C230.zxyw, r2.zxyw);
  r6.w = AvDot4(C231.zxyw, r2.zxyw);
  precise vec3 offset = r2.xyz + (-C59.xyz);
  r4.xyz = offset;
  r0.w = inversesqrt(abs(r0.y));
  r0.y = AvDot3(r4.zxy, r4.zxy);
  r5.x = AvDot4(r6.wzxy, C56.wzxy);
  r5.y = AvDot4(r6.wzxy, C57.wzxy);
  r5.w = AvDot4(r6.wzxy, C59.wzxy);
  r0.z = AvDot4(r6.wzxy, C58.wzxy);
  r2.z = AvMulX(C202.z, r0.w);
  r2.y = AvMulX(C202.y, r0.w);
  r2.x = AvMulX(C202.x, r0.w);
  precise float depth = r0.z + (-C200.x);
  r5.z = depth;
  r0.y = inversesqrt(abs(r0.y));
  r4.xyz = vec3(AvMulX(r4.x, r0.y), AvMulX(r4.y, r0.y), AvMulX(r4.z, r0.y));
  bool p0 = r0.x != 0.0;
  if (p0) {
    r0.x = AvOne(r0.x);
  } else {
    precise vec3 base = C230.xyz * C255.x;
    r0.xyz = base;
    precise float t0 = AvMulX(r4.z, C230.z);
    precise float t1 = AvMulX(r4.x, C230.x);
    precise float t2 = AvMulX(r4.y, C230.y);
    precise float s01 = t0 + t1;
    precise float dotv = s01 + t2;
    r0.w = dotv;
    r0.xyz = (-r0.w > 0.0) ? r0.xyz : C230.xyz;
    precise float u0 = AvMulX(r2.z, r0.z);
    precise float u1 = AvMulX(r2.x, r0.x);
    precise float u2 = AvMulX(r2.y, r0.y);
    precise float us = u0 + u1;
    precise float ud = us + u2;
    r0.x = ud;
    r0.x = r0.x >= C255.z ? r0.x : C255.z;
    precise float doubled = r0.x + r0.x;
    r0.x = isnan(doubled) ? 0.0 : clamp(doubled, 0.0, 1.0);
  }
  r1.xyz = vec3(AvMulX(r1.x, r0.x), AvMulX(r1.y, r0.x), AvMulX(r1.z, r0.x));

  XeOutputPosition(r5);
  out_a = vec4(r3.z, r3.w, 0.0, 0.0);
  out_b = vec4(r3.x, r3.y, 0.0, 0.0);
  out_color = r1;
}
