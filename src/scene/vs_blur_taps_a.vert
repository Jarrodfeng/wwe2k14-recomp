#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for one of the game's shadow-blur quads, the yBlur kernel
// (guest vertex shader BDFD77B5991F86C8; 1 / 1 / 1 draws a frame in Brock's entrance / Bryan's entrance / the match). Pixel side: the
// shadow blur pixel shaders (bench census). Hand-written from our reading of the guest shader; checked bit for bit against the SDK's
// translation by tools/shaders/harness/vs_harness.py.
//
// A 3-float vertex (3 dwords: x, y, z; the quad lies in x and z). The corner is scaled by c40 (x by c40.x, z by c40.z); the blur's
// half width (c33.x x the literal c255.y) is added to and taken from x. Three interpolators carry a texture-coordinate quadruple each,
// for the corner, the corner shifted by minus the half width and by plus it: a quadruple is the four rows c36 / c37 / c38 / c39 against
// (x, c40.y, z) (the x term rounded, the c40.y term fused in, the z term added), plus the row's w. The clip position is the first
// quadruple with its z scaled by the literal c255.x.
// Float constants (set 1 binding 1), packed: 33 36 37 38 39 40 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[7]; } fc;
#define C33 fc.c[0]
#define C36 fc.c[1]
#define C37 fc.c[2]
#define C38 fc.c[3]
#define C39 fc.c[4]
#define C40 fc.c[5]
#define C255 fc.c[6]

#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_corner;
layout(location = 1) invariant out vec4 out_minus;
layout(location = 2) invariant out vec4 out_plus;

// One row against (t, c40.y, z): t x row.x rounded, + c40.y x row.y fused, + z x row.z, + the row's w (summed in the guest's order).
float RowTerm(vec4 row, float t, float z) {
  precise float scaled = t * row.x;
  float mixed = fma(C40.y, row.y, scaled);
  precise float with_z = mixed + z * row.z;
  return with_z;
}
vec4 Quadruple(float t, float z) {
  precise float a = C36.w + RowTerm(C36, t, z);
  precise float b = RowTerm(C37, t, z) + C37.w;
  precise float c = RowTerm(C38, t, z) + C38.w;
  precise float d = RowTerm(C39, t, z) + C39.w;
  return vec4(a, b, c, d);
}

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 3, 95);
  float vx = XeFetchFloat(at), vz = XeFetchFloat(at + 2);

  precise float half_width = C33.x * C255.y;
  precise float x = vx * C40.x;
  precise float z = vz * C40.z;
  precise float x_plus = x + half_width;
  precise float x_minus = x + (-half_width);

  vec4 corner = Quadruple(x, z);
  precise float clip_z = corner.z * C255.x;
  XeOutputPosition(vec4(corner.x, corner.y, clip_z, corner.w));
  out_corner = corner;
  out_minus = Quadruple(x_minus, z);
  out_plus = Quadruple(x_plus, z);
}
