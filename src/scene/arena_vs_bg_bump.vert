#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena's bump-mapped, lightmapped material, the game's
// yBG_BumpMapLM (guest vertex shader 54C931B2B2BDF71B; 1 / 1 / 0 draws a frame in Brock's entrance / Bryan's entrance / the match).
// Hand-written from our reading of the guest shader; checked bit for bit against the SDK's translation by
// tools/shaders/harness/vs_harness.py.
//
// The arena vertex (8 dwords: position, a packed normal, a colour, a texture coordinate; no skinning): the position and the normal
// through the model matrix rows c76 / c77 / c78 (the rows are taken against the position summed w, z, x, y and against the normal summed
// z, x, y, as the arena shaders do), the clip position, and the pieces the bump-mapped pixel shader needs. Interpolators:
//   0 = (u, v, 1); 1 = the camera (c72) less the world position; 2 = the negated turned normal; 3 and 4 = the tangent frame, built
//   one of two ways by whether |normal y| is under the threshold c255.z (a flag in each when the world z is not below zero);
//   5 = the world position through the rows c68 / c69 / c166 / c71; 6 = (flag, flag); 7 = the lighting-map coordinate
//   ((row 76, row 78) x c184 x c255.z + c255.z); 8 = the vertex colour (alpha x c185.x x c189.w x c190.w).
// Float constants (set 1 binding 1), packed: 56 57 58 59 68 69 71 72 76 77 78 166 184 185 189 190 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[17]; } fc;
#define C56 fc.c[0]
#define C57 fc.c[1]
#define C58 fc.c[2]
#define C59 fc.c[3]
#define C68 fc.c[4]
#define C69 fc.c[5]
#define C71 fc.c[6]
#define C72 fc.c[7]
#define C76 fc.c[8]
#define C77 fc.c[9]
#define C78 fc.c[10]
#define C166 fc.c[11]
#define C184 fc.c[12]
#define C185 fc.c[13]
#define C189 fc.c[14]
#define C190 fc.c[15]
#define C255 fc.c[16]

#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_view;
layout(location = 2) invariant out vec4 out_normal;
layout(location = 3) invariant out vec4 out_tangent_a;
layout(location = 4) invariant out vec4 out_tangent_b;
layout(location = 5) invariant out vec4 out_projected;
layout(location = 6) invariant out vec4 out_flag;
layout(location = 7) invariant out vec4 out_lightmap;
layout(location = 8) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 8, 95);
  vec4 position = AvFetchPosition(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColorAt(at + 6);
  vec2 halves = AvFetchHalves(at);

  // the model matrix: rows 76 / 77 / 78 -> x / y / z
  vec4 q = position.wzxy;
  float wx = AvDot4(q, C76.wzxy), wy = AvDot4(q, C77.wzxy), wz = AvDot4(q, C78.wzxy);
  vec3 n = normal.zxy;
  float tz = AvDot3(n, C78.zxy);
  float ty = AvDot3(n, C77.zxy);
  float tx_negated = AvDot3(-n, C76.zxy);  // (the negated normal against row 76)

  float steep = (C255.z > abs(ty)) ? 1.0 : 0.0;
  precise float alpha_scale = C190.w * C189.w;
  precise float alpha = C185.x * alpha_scale;
  precise float color_alpha = color.w * alpha;

  vec4 p = vec4((C255.x == 0.0) ? wz : C255.y, (C255.x == 0.0) ? wx : C255.y, (C255.x == 0.0) ? wy : C255.y,
                (C255.y == 0.0) ? wy : C255.y);
  XeOutputPosition(vec4(AvDot4(C56.zxyw, p), AvDot4(C57.zxyw, p), AvDot4(C58.zxyw, p), AvDot4(C59.zxyw, p)));

  precise vec3 view = vec3(C72.x - wx, C72.y - wy, C72.z - wz);
  float flag = ((-abs(wz)) > C255.x) ? 1.0 : 0.0;

  // the tangent frame
  vec3 frame_a, frame_b;
  if (steep != 0.0) {
    precise float square = tz * tz;
    precise float length2 = fma(tx_negated, tx_negated, square);
    float inv = inversesqrt(abs(length2));
    float u = AvMulX(tz, inv);
    float g = AvMulX(tx_negated, inv);
    precise float q0 = AvMulX(u, -ty);
    precise float q1 = AvMulX(u, -tz);
    precise float q2 = AvMulX(g, tx_negated) + (-q1);
    frame_a = vec3(u, flag, g);
    frame_b = vec3(AvMulX(-ty, g), -q2, -q0);
  } else {
    precise float a = (-tz) * C255.w;
    precise float b = (-ty) * C255.w;
    precise float square = a * a;
    precise float length2 = fma(-b, -b, square);
    float inv = inversesqrt(abs(length2));
    float x5 = AvMulX(a, inv);
    float y5 = AvMulX(inv, -b);
    float z1 = AvMulX(x5, tx_negated);
    precise float t1 = AvMulX(x5, -tz);
    precise float t2 = AvMulX(y5, -ty);
    precise float x1 = t1 + (-t2);
    float y1 = AvMulX(tx_negated, y5);
    frame_a = vec3(-x1, -y1, z1);
    frame_b = vec3(flag, x5, y5);
  }

  out_uv = vec4(halves.y, halves.x, 1.0, 0.0);
  out_view = vec4(view, 0.0);
  out_normal = vec4(tx_negated, -ty, -tz, 0.0);
  out_tangent_a = vec4(frame_a, 0.0);
  out_tangent_b = vec4(frame_b, 0.0);
  out_projected = vec4(AvDot4(C68.zxyw, p), AvDot4(C69.zxyw, p), AvDot4(C166.zxyw, p), AvDot4(C71.zxyw, p));
  out_flag = vec4(flag, flag, 0.0, 0.0);
  precise float scale_u = C184.x * C255.z;
  precise float scale_v = C184.y * C255.z;
  out_lightmap = vec4(fma(scale_u, wx, C255.z), fma(scale_v, wz, C255.z), 0.0, 0.0);
  out_color = vec4(color.rgb, color_alpha);
}
