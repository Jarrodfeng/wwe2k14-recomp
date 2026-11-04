#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's 2D rectangle, the game's yRect (guest
// vertex shader 2D1389E128EA124E, a vertex-declaration copy of base shader AFD5268C...; 12 / 15 / 2 draws a frame in Brock's
// entrance / Bryan's entrance / the match: the crowd atlas copy and the titantron's colour conversion). Pixel shaders:
// B57EADC321958E8F, 7ABDFEFBCADB4BF9. Written from our reading of the guest microcode; checked against the translation by
// tools\shaders\harness\vs_harness.py.
//
// 5-dword vertices: a position (3 floats) and a texture coordinate (2 floats). Two ways to the clip position, by a compare of
// two constants (c255.x against c28.x): equal - the screen-space path: (x, y) offset by c255.z, scaled by c0.xy and turned
// clip (x, y) = (-c255.y + x', -(y' - c255.y)) as the guest writes it, z the fetched z, w from the 'at least' compare;
// otherwise the position through the matrix c29..c32. Interpolators: 0 = the
// coordinate (xy); 1 = c1 (the tint).
// Float constants (set 1 binding 1, packed in register order): 0 1 28 29 30 31 32 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[8]; } fc;
#define C0    fc.c[0]
#define C1    fc.c[1]
#define C28   fc.c[2]
#define C29   fc.c[3]
#define C30   fc.c[4]
#define C31   fc.c[5]
#define C32   fc.c[6]
#define C255  fc.c[7]
#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_color;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex(), 5, 95);
  vec4 r0 = vec4(0.0), r1 = vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), 0.0), r2;
  r0.zw = vec2(XeFetchFloat(at + 3), XeFetchFloat(at + 4));
  r0.x = (C255.x != C28.x) ? 1.0 : 0.0;
  if (r0.x == 0.0) {
    precise vec2 shifted = r1.xy + C255.zz;
    precise vec2 scaled = shifted * C0.xy;
    r0.xy = scaled;
    precise float below = r0.y + (-C255.y);
    float one = AvOne(r0.x);
    r1.x = below;
    r1.w = one;
    precise float across = (-C255.y) - (-r0.x);
    r1.y = -r1.x;
    r1.x = across;
  } else {
    r2 = vec4((C255.x == 0.0) ? r1.z : C255.y, (C255.x == 0.0) ? r1.x : C255.y, (C255.x == 0.0) ? r1.y : C255.y,
              (C255.y == 0.0) ? r1.y : C255.y);
    r1.x = AvDot4(C29.zxyw, r2);
    r1.y = AvDot4(C30.zxyw, r2);
    r1.z = AvDot4(C31.zxyw, r2);
    r1.w = AvDot4(C32.zxyw, r2);
  }
  XeOutputPosition(r1);
  out_uv = vec4(r0.z, r0.w, 0.0, 0.0);
  out_color = C1;
}
