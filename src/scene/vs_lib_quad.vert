#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's library textured quad (copies and the post
// chain's passes; guest vertex shader 1456901233B6564C, a vertex-declaration copy of the library shader at 0x820B4700; 20 / 22
// / 13 draws a frame in Brock's entrance / Bryan's entrance / the match). Written from our reading of the guest microcode;
// checked against the translation by tools\shaders\harness\vs_harness.py.
//
// 6-dword vertices: a position (x, y, z, w) and a texture coordinate (2 floats at dword 4), passed through as the first
// interpolator (xy). No float constants.


layout(location = 0) invariant out vec4 out_uv;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 6, 95);
  XeOutputPosition(vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), XeFetchFloat(at + 3)));
  out_uv = vec4(XeFetchFloat(at + 4), XeFetchFloat(at + 5), 0.0, 0.0);
}
