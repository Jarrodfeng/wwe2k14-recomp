#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's moded sprite, the game's ySprite (guest
// vertex shader 3FCE6872E9C04D68, a vertex-declaration copy of base shader CD4D8EA3...; 11 draws in Brock's entrance: the
// video-with-alpha plane and the overlays). Pixel shader: 7754C61815DA6614. Written from our reading of the guest microcode;
// checked against the translation by tools\shaders\harness\vs_harness.py.
//
// vs_rect.vert's position and coordinate (here c28 is the compared constant, c0 the scale, c29..c32 the matrix), plus the
// sprite's mode masks from c4 against c255: interpolator 1 = c4 where the mode's test (c4 > c255.x and c255.w >= c4) fails,
// else c255.y; interpolator 3.x = the guest's chain of "first nonzero" selects over the four tests. Interpolators: 0 = the
// coordinate (xy); 1 = masked c4; 2 = c1 (the tint); 3 = (select, 0, 0, 0).
// Float constants (set 1 binding 1, packed in register order): 0 1 4 28 29 30 31 32 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[9]; } fc;
#define C0    fc.c[0]
#define C1    fc.c[1]
#define C4    fc.c[2]
#define C28   fc.c[3]
#define C29   fc.c[4]
#define C30   fc.c[5]
#define C31   fc.c[6]
#define C32   fc.c[7]
#define C255  fc.c[8]
#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_a;
layout(location = 2) invariant out vec4 out_tint;
layout(location = 3) invariant out vec4 out_b;

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
  r2 = vec4(greaterThan(C4, vec4(C255.x)));
  r1 = vec4(greaterThanEqual(vec4(C255.w), C4));
  precise vec4 both = r2 * r1;
  r1 = both;
  out_uv = vec4(r0.z, r0.w, 0.0, 0.0);
  out_a = vec4((r1.x == 0.0) ? C4.x : C255.y, (r1.y == 0.0) ? C4.y : C255.y, (r1.z == 0.0) ? C4.z : C255.y,
               (r1.w == 0.0) ? C4.w : C255.y);
  r0.x = (r1.x != 0.0) ? 1.0 : 0.0;
  out_tint = C1;
  r0.x = (r1.y == 0.0) ? r0.x : C255.y;
  r0.x = (r1.z == 0.0) ? r0.x : C255.y;
  out_b = vec4((r1.w == 0.0) ? r0.x : C255.y, 0.0, 0.0, 0.0);
}
