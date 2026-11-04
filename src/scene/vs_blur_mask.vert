#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
#include "xenos_vs_tex.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for a blur pass with a vertex-side mask lookup, the game's
// yBlur (guest vertex shader 20395ADD7E10418F, a vertex-declaration copy of base shader C82B85F5...; 3 / 3 / 3 draws a frame).
// Pixel shaders: the shadow blurs 632189BAFAE233F0, 7C650AA1C792DFF6, 694A6D148DC33CFF. Written from our reading of the guest
// microcode; checked against the translation by tools\\shaders\\harness\\vs_harness.py.
//
// 6-dword vertices: a 2-float position (dwords 0-1) and a coordinate (floats at 4-5). The coordinate reads one channel of the vertex
// texture of fetch constant 16 (xenos_vs_tex.glsl); a nonzero value keeps the quad (position x, y times 1) and zero collapses it
// (w = 0): clip = (x, y) x flag, z = 1, w = flag, with (x, y) = position x c32.xy + c32.zw. Interpolator 0 = the coordinate (xy).
// Float constants (set 1 binding 1, packed in register order): 32.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C32   fc.c[0]
#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_uv;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 6, 95);
  vec4 r0 = vec4(XeFetchFloat(at + 4), XeFetchFloat(at + 5), XeFetchFloat(at), XeFetchFloat(at + 1));
  float mask = XeVertexTexture16(r0.xy).x;
  float flag = (mask != 0.0) ? 1.0 : 0.0;
  vec2 placed = AvFma(r0.zw, C32.xy, C32.zw);
  precise vec2 scaled = placed * flag;
  XeOutputPosition(vec4(scaled, 1.0, flag));
  out_uv = vec4(r0.xy, 0.0, 0.0);
}
