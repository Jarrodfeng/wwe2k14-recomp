#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the post chain's glare quad, the game's yGlareEtc
// (guest vertex shader D6B8DEB0775CC47F; 1 draw a frame in each scene).
// Pixel shaders: the glare combines (4E2CD8031D93DE3E, 1233F68C15FDF948). Written from our reading of the guest microcode;
// checked against the translation by tools\shaders\harness\vs_harness.py.
//
// As vs_lib_quad3.vert (w = 1), the coordinate also offset by c255.xy into the interpolator's z and w. Interpolator 0 =
// (u, v, u + c255.x, v + c255.y).
// Float constants (set 1 binding 1, packed in register order): 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C255  fc.c[0]

layout(location = 0) invariant out vec4 out_uv;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 6, 95);
  XeOutputPosition(vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), 1.0));
  vec2 uv = vec2(XeFetchFloat(at + 4), XeFetchFloat(at + 5));
  precise vec2 shifted = uv + C255.xy;
  out_uv = vec4(uv, shifted);
}
