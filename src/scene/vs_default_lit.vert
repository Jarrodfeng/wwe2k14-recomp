#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's lit default mesh, the game's yDefault (guest
// vertex shader 3834DE7318E253D5, a vertex-declaration copy of base shader 0B023F5C...; 4 / 4 / 0 draws a frame in Brock's / Bryan's
// entrance). Pixel shader: B5853824AE01D2D8 in the bench census. Written from our reading of the guest microcode; checked against
// the translation by tools\\shaders\\harness\\vs_harness.py.
//
// The arena vertex (8 dwords: position, one bone byte at dword 4, packed normal, colour, texture coordinate) with the bool constant
// b0 choosing one skin bone (c[76 + 3 x byte]; the whole float block is indexed - kDynamic) or the plain model rows c76..c78. The
// lights' directions are scaled by the reciprocal root of the turned normal's squared length, so the dot products give normalised
// cosines; the arena's lighting (arena_vs.glsl) follows. Interpolators: 0 = (u, v); 1 = the scaled negated normal; 2 = camera
// (c72) less world position; 3 = zeros; 4 / 5 = (cmp, cmp, cmp / 0, 0) with cmp = (-|normal squared| > c255.x); 6 = colour
// (rgb = vertex colour x light, a = vertex alpha x c185.x x c189.w x c190.w).
// Float constants (set 1 binding 1): all 256 registers by number.

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_normal;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_zero;
layout(location = 4) invariant out vec4 out_flag_a;
layout(location = 5) invariant out vec4 out_flag_b;
layout(location = 6) invariant out vec4 out_color;

#define C0   fc.c[0]
#define C1   fc.c[1]
#define C2   fc.c[2]
#define C3   fc.c[3]
#define C8   fc.c[8]
#define C9   fc.c[9]
#define C10  fc.c[10]
#define C11  fc.c[11]
#define C72  fc.c[72]
#define C74  fc.c[74]
#define C76  fc.c[76]
#define C77  fc.c[77]
#define C78  fc.c[78]
#define C180 fc.c[180]
#define C181 fc.c[181]
#define C182 fc.c[182]
#define C185 fc.c[185]
#define C189 fc.c[189]
#define C190 fc.c[190]

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 8, 95);
  vec4 r2 = vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), XeFetchFloat(at + 3));
  float bone_byte = float(bitfieldExtract(XeFetchDword(at + 4), 0, 8));
  vec3 normal = AvFetchNormal(at);
  vec4 r0 = vec4(trunc(bone_byte), normal);
  vec4 color = AvFetchColor(at);
  vec2 halves = AvFetchHalves(at);
  vec3 world, turned;
  vec3 q = vec3(r0.w, r0.y, r0.z);
  if (XeBool(0)) {
    precise float scaled = C255.w * r0.x;
    int a = XeAddress(scaled);
    world = vec3(AvDot4(r2.wzxy, fc.c[76 + XeBone(a)].wzxy), AvDot4(r2.wzxy, fc.c[77 + XeBone(a)].wzxy), AvDot4(r2.wzxy, fc.c[78 + XeBone(a)].wzxy));
    turned = vec3(AvDot3(q, fc.c[76 + XeBone(a)].zxy), AvDot3(q, fc.c[77 + XeBone(a)].zxy), AvDot3(q, fc.c[78 + XeBone(a)].zxy));
  } else {
    world = vec3(AvDot4(r2.wzxy, C76.wzxy), AvDot4(r2.wzxy, C77.wzxy), AvDot4(r2.wzxy, C78.wzxy));
    turned = vec3(AvDot3(q, C76.zxy), AvDot3(q, C77.zxy), AvDot3(q, C78.zxy));
  }
  vec4 pos = vec4((C255.x == 0.0) ? world.z : C255.y, (C255.x == 0.0) ? world.x : C255.y, (C255.x == 0.0) ? world.y : C255.y,
                  (C255.y == 0.0) ? world.y : C255.y);
  XeOutputPosition(vec4(AvDot4(C56.zxyw, pos), AvDot4(C57.zxyw, pos), AvDot4(C58.zxyw, pos), AvDot4(C59.zxyw, pos)));

  vec3 n = turned.zxy;
  float len2 = AvDot3(n, n);
  float k = inversesqrt(abs(len2));
  float cmp = (-abs(len2) > C255.x) ? 1.0 : 0.0;
  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  out_zero = vec4(0.0);
  out_flag_a = vec4(cmp, cmp, 0.0, 0.0);
  out_flag_b = vec4(cmp, cmp, cmp, 0.0);
  precise vec3 view = (-world) + C72.xyz;
  out_view = vec4(view, 0.0);
  out_normal = vec4(AvMulX3(vec3(-k), turned), 0.0);

  // The lights' directions scaled by the reciprocal root, dotted with the turned normal in the order z, x, y.
  float d0 = AvDot3R(AvMulX3(vec3(k), vec3(C8.z, C8.x, C8.y)), n);
  float d3 = AvDot3R(AvMulX3(vec3(k), vec3(C11.z, C11.x, C11.y)), n);
  float d2 = AvDot3R(AvMulX3(vec3(k), vec3(C10.z, C10.x, C10.y)), n);
  float d1 = AvDot3R(AvMulX3(vec3(k), vec3(C9.z, C9.x, C9.y)), n);
  vec3 up_axis = AvMulX3(vec3(-k), C180.xyz);
  float up = AvDot3R(vec3(up_axis.z, up_axis.x, up_axis.y), n);
  vec4 t = AvMax4(vec4(d3, d2, d1, d0), vec4(C74.w, C74.z, C74.y, C74.x));
  precise vec3 span = C182.xyz - C181.xyz;
  precise float up_scaled = AvMulX(up, C255.z);
  precise float facing = up_scaled + C255.z;
  precise vec3 blended = AvMulX3(span, vec3(facing)) + C181.xyz;
  vec3 ambient = AvMulX3(blended, C189.xyz);
  precise vec3 sum = AvMulX3(vec3(t.x), C3.xyz);
  precise vec3 sum1 = AvMulX3(vec3(t.y), C2.xyz) + sum;
  precise vec3 sum2 = AvMulX3(vec3(t.z), C1.xyz) + sum1;
  precise vec3 sum3 = AvMulX3(vec3(t.w), C0.xyz) + sum2;
  precise vec3 lit = AvMulX3(sum3, C190.xyz) + ambient;
  precise float alpha_base = C189.w * C190.w;
  precise float alpha_scale = alpha_base * C185.x;
  out_color = vec4(AvMulX3(lit, color.rgb), AvMulX(alpha_scale, color.w));
}
