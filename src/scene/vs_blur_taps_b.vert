#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for one of the game's shadow-blur quads, the yBlur kernel
// (guest vertex shader 1E2294D64E4D2958; 1 / 1 / 1 draws a frame in Brock's entrance / Bryan's entrance / the match). Pixel side: the
// shadow blur pixel shaders (bench census). Hand-written from our reading of the guest shader; checked bit for bit against the SDK's
// translation by tools/shaders/harness/vs_harness.py.
//
// A 3-float vertex (3 dwords: x, y, z; the quad lies in x and z). The quad's corner is scaled by c40 (x by c40.x, z by c40.z) and the
// blur's half width (c33.x x the literal c255.z) added to or taken from z. Three interpolators carry a texture-coordinate quadruple each:
// every one of the four numbers is a 3-term dot product of a "tap" (x, z, c40.y-ish) against one of the rows c36 / c37 / c38 / c39, plus
// that row's w; the taps are the corner shifted by none, +, or - the half width. The clip position takes the first interpolator's x / y / w
// and the z of the third row scaled by the literal c255.w.
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

layout(location = 0) invariant out vec4 out_a;
layout(location = 1) invariant out vec4 out_b;
layout(location = 2) invariant out vec4 out_c;

// A slot of a tap: the coordinate itself when the literal register's x is 0, else c40.y (the guest's compare-and-select).
float Slot0(float v) { return (C255.x == 0.0) ? v : C40.y; }
float Slot1(float v) { return (C255.y == 0.0) ? v : C40.y; }

// The row `r` (c36 + r) against a tap, summed z, x, y of the row (fused).
float Row(vec3 tap, vec4 row) { return AvDot3(tap, row.zxy); }

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 3, 95);
  float vx = XeFetchFloat(at), vz = XeFetchFloat(at + 2);

  precise float half_width = C33.x * C255.z;
  precise float x = vx * C40.x;
  precise float z = vz * C40.z;
  precise float z_minus = z + (-half_width);
  precise float z_plus = z + half_width;

  // the taps: (z-ish, x, last) as the rows take them
  vec3 t_z = vec3(Slot0(z), Slot0(x), Slot1(z));              // centre
  vec3 t_zp_z = vec3(Slot0(z_plus), Slot0(x), Slot1(z));
  vec3 t_z_zm = vec3(Slot0(z), Slot0(x), Slot1(z_minus));
  vec3 t_zm = vec3(Slot0(z_minus), Slot0(x), Slot1(z_minus));
  vec3 t_z_zp = vec3(Slot0(z), Slot0(x), Slot1(z_plus));
  vec3 t_zp = vec3(Slot0(z_plus), Slot0(x), Slot1(z_plus));
  vec3 t_zm_zp = vec3(Slot0(z_minus), Slot0(x), Slot1(z_plus));
  vec3 t_zp_zm = vec3(Slot0(z_plus), Slot0(x), Slot1(z_minus));

  precise float a_x = C36.w + Row(t_z_zp, C36);
  precise float a_y = Row(t_z_zm, C37) + C37.w;
  precise float a_z = Row(t_z, C38) + C38.w;
  precise float a_w = Row(t_z_zm, C39) + C39.w;
  precise float b_x = C36.w + Row(t_zp, C36);
  precise float b_y = Row(t_zp, C37) + C37.w;
  precise float b_z = Row(t_zp_zm, C38) + C38.w;
  precise float b_w = Row(t_zp_z, C39) + C39.w;
  precise float c_x = C36.w + Row(t_zm_zp, C36);
  precise float c_y = Row(t_zm, C37) + C37.w;
  precise float c_z = Row(t_zm, C38) + C38.w;
  precise float c_w = Row(t_zm, C39) + C39.w;

  precise float clip_z = a_z * C255.w;
  XeOutputPosition(vec4(a_x, a_y, clip_z, a_w));
  out_a = vec4(a_x, a_y, a_z, a_w);
  out_b = vec4(b_x, b_y, b_z, b_w);
  out_c = vec4(c_x, c_y, c_z, c_w);
}
