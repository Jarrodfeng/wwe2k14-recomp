#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's library textured quad with a three-float
// position (guest vertex shader 2C132E0B6039C3F8, a vertex-declaration copy of the library shader at 0x82082A38; 1 / 3 / 0
// draws a frame). Pixel side: the restore copy (9A836B68E3F8F0B0). Written from our reading of the guest microcode; checked
// against the translation by tools\shaders\harness\vs_harness.py.
//
// As vs_lib_quad.vert but the position's w is 1 (the vertex's fourth float is not read). Interpolator 0 = the coordinate (xy).


layout(location = 0) invariant out vec4 out_uv;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 6, 95);
  XeOutputPosition(vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), 1.0));
  out_uv = vec4(XeFetchFloat(at + 4), XeFetchFloat(at + 5), 0.0, 0.0);
}
