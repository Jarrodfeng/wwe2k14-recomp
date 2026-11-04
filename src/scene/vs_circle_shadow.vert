#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the wrestlers' circle shadows, the game's yCircleShadow (guest
// vertex shader 63170EAB428C7EFA, a vertex-declaration copy of base shader 2AF027A9...; 3 / 5 / 5 draws a frame). Pixel side:
// circle_shadow.frag (guest pixel shader D05BFEA12F4350AD). Written from our reading of the guest microcode; checked against the
// translation by tools\\shaders\\harness\\vs_harness.py.
//
// 8-dword vertices: a position (3 floats), a float (dword 3), two bytes (dword 4: two offset-table indices) and a 3-float value
// (dwords 5-7). The position is the vertex plus the float times an entry of the offset table at c[8 + index], twice, through the
// rows c4..c7; the shadow fades with the distance (c0.y, c0.z) and a power curve (log2 / exp2 with c0.w, c0.x). The quad is
// dropped (clip x 0) where the result is below c255.z. Interpolators: 0 = the value's xy; 1 = (fade, 0, 0, 0).
// Float constants (set 1 binding 1): all 256 registers by number.

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"

layout(location = 0) invariant out vec4 out_a;
layout(location = 1) invariant out vec4 out_b;

#define C0 fc.c[0]
#define C4 fc.c[4]
#define C5 fc.c[5]
#define C6 fc.c[6]
#define C7 fc.c[7]

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 8, 95);
  vec3 r3p = vec3(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2));
  float w3 = XeFetchFloat(at + 3);
  uint bytes = XeFetchDword(at + 4);
  vec2 offsets = vec2(bitfieldExtract(bytes, 0, 8), bitfieldExtract(bytes, 8, 8));
  vec3 r2 = vec3(XeFetchFloat(at + 5), XeFetchFloat(at + 6), XeFetchFloat(at + 7));
  vec2 r1xy = trunc(offsets);
  int a = XeAddress(r1xy.x);
  float flag = (abs(float(index)) >= C255.y) ? 1.0 : 0.0;
  vec3 moved = fma(vec3(w3), fc.c[8 + a].xyz, r3p);
  precise float rest = (-w3) + C255.x;
  a = XeAddress(r1xy.y);
  vec3 placed = fma(vec3(rest), fc.c[8 + a].yxz, vec3(moved.y, moved.x, moved.z));
  vec4 v = vec4(placed.z, placed.y, placed.x, flag);
  vec4 r3 = vec4(AvDot4(C4.zxyw, v), AvDot4(C5.zxyw, v), AvDot4(C6.zxyw, v), AvDot4(C7.zxyw, v));
  precise float dist = (-C0.y) - (-placed.x);
  precise float inv = 1.0 / C0.z;
  float faded = AvMulX(abs(dist), inv);
  faded = isnan(faded) ? 0.0 : clamp(faded, 0.0, 1.0);
  precise float remaining = C255.x - faded;
  float lg = log2(abs(remaining));
  vec2 curve = AvMulX(vec2(lg, r2.z), vec2(C0.w, C0.x));
  float power = exp2(curve.x);
  float shadow = AvMulX(curve.y, power);
  float keep = (shadow >= C255.z) ? 1.0 : 0.0;
  vec4 clip = vec4(AvMulX(r3.x, keep), AvMulX(r3.y, keep), AvMulX(r3.z, keep), AvMulX(r3.w, keep));
  XeOutputPosition(clip);
  out_a = vec4(r2.xy, 0.0, 0.0);
  out_b = vec4(shadow, 0.0, 0.0, 0.0);
}
