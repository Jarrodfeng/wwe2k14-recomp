#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the entrance's lighting cubes, the game's yPreLight
// (guest vertex shader D261C3CC487FA436, a vertex-declaration copy of base shader B799CCB8...; 5 draws in Bryan's entrance).
// Pixel side: side_prelight.frag (guest pixel shader 8479D8DA0BB01D6B). Written from our reading of the guest microcode;
// checked against the translation by tools\shaders\harness\vs_harness.py.
//
// 12-dword vertices: a position (3 floats, w = 1) and a 3-float value passed on. The position is already in clip space; x and y
// are scaled and offset by c201 (xy scale, zw offset). Interpolator 0 = the value (xyz).
// Float constants (set 1 binding 1, packed in register order): 201.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C201  fc.c[0]
#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_a;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 12, 95);
  vec4 r1 = vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), 1.0);
  vec2 xy = AvFma(r1.xy, C201.xy, C201.zw);
  XeOutputPosition(vec4(xy, r1.z, r1.w));
  out_a = vec4(XeFetchFloat(at + 3), XeFetchFloat(at + 4), XeFetchFloat(at + 5), 0.0);
}
