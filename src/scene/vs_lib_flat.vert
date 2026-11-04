#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's library flat-colour primitive (the clears and
// copies' rectangle; guest vertex shader 1E6883FCCDE1F688, the library's own shader read from the executable, not a patched
// copy; 40 / 41 / 37 draws a frame in Brock's entrance / Bryan's entrance / the match). Pixel side: debug_primitive.frag (guest
// pixel shader A4A965C189287B99). Written from our reading of the guest microcode; checked against the translation by
// tools\shaders\harness\vs_harness.py.
//
// The one vertex stream is in vertex fetch constant 0 here (7 dwords a vertex): a position (x, y, z, w = 1) and a
// 4-float value passed through as the first interpolator. No float constants.


layout(location = 0) invariant out vec4 out_a;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 7, 0);
  XeOutputPosition(vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), 1.0));
  out_a = vec4(XeFetchFloat(at + 3), XeFetchFloat(at + 4), XeFetchFloat(at + 5), XeFetchFloat(at + 6));
}
