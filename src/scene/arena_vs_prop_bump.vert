#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena props' normal-mapped material, the game's
// yProp_BumpMap (guest vertex shader 13B1BADF537A2BFF, a vertex-declaration copy of base shader 4069AD32...; 35 / 35 / 35 draws
// a frame in Brock's entrance / Bryan's entrance / the match). Pixel side: arena_prop_bump.frag (guest pixel shader
// 2B8A869CF01395DB). Written from our reading of the guest microcode; checked against the translation by
// tools\shaders\harness\vs_harness.py.
//
// The vertex is the arena's (8 dwords: position, packed normal, colour, texture coordinate). The shader builds the surface's
// frame for the normal map from the model-turned normal: a tangent perpendicular to it, picked by a per-vertex test (the
// normal against c77 compared with c255.x: a different cross product when it lies close to the vertical axis), and the
// bitangent from the cross product. It is written with the guest's own registers (r0..r7), as the two branches reuse them.
// Interpolators: 0 = (u, v, 1); 1 = view vector (c72 less the world position, in the model rows' axis order); 2 = the
// negated turned normal; 3 = the frame's first axis; 4 = the frame's second axis (xyz taken in the order z, x, y); 5 = the
// vertex colour with its alpha times c185.x x c189.w x c190.w. The 'degenerate' compare of the guest (-|length squared| > c255.y)
// is kept in the lanes it leaves it in.
// Float constants (set 1 binding 1, packed in register order): 56 57 58 59 72 76 77 78 185 189 190 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[12]; } fc;
#define C56   fc.c[0]
#define C57   fc.c[1]
#define C58   fc.c[2]
#define C59   fc.c[3]
#define C72   fc.c[4]
#define C76   fc.c[5]
#define C77   fc.c[6]
#define C78   fc.c[7]
#define C185  fc.c[8]
#define C189  fc.c[9]
#define C190  fc.c[10]
#define C255  fc.c[11]
#include "xenos_vs_math.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_view;
layout(location = 2) invariant out vec4 out_normal;
layout(location = 3) invariant out vec4 out_a;
layout(location = 4) invariant out vec4 out_b;
layout(location = 5) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index);
  vec4 position = AvFetchPosition(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColor(at);
  vec2 halves = AvFetchHalves(at);

  // The guest's registers, named as it names them: r0 holds the normal in y, z, w (and the vertex index in x until it is
  // reused), r1 the world position, r2 the clip position, r3 / r7 the normal turned by the model rows, r5 the colour.
  vec4 r0 = vec4(float(index), normal), r1 = vec4(0.0), r2, r3 = vec4(0.0), r4, r5 = color, r6 = vec4(0.0), r7 = vec4(0.0);
  float ps;

  r7.x = AvDot3(-vec3(r0.w, r0.y, r0.z), C76.zxy);
  r1.y = AvDot4(position.wzxy, C76.wzxy);
  r1.x = AvDot4(position.wzxy, C78.wzxy);
  r1.z = AvDot4(position.wzxy, C77.wzxy);
  ps = C189.w;
  r3.y = AvDot3(vec3(r0.w, r0.y, r0.z), C77.zxy);
  precise float alpha_scale = C190.w * ps;
  r0.x = alpha_scale;
  r1.w = AvOne(r0.x);
  precise float scale = r0.x * C185.x;
  r0.x = scale;
  r3.x = AvDot3(vec3(r0.w, r0.y, r0.z), C78.zxy);
  ps = r0.x;
  r7.y = -r3.y;
  r7.z = -r3.x;
  precise float alpha = r5.w * ps;
  r5.w = alpha;

  r0.x = (C255.x > abs(r3.y)) ? 1.0 : 0.0;
  r0.y = r1.y;
  r2.x = AvDot4(C56.zxyw, r1);
  r2.y = AvDot4(C57.zxyw, r1);
  r2.z = AvDot4(C58.zxyw, r1);
  r2.w = AvDot4(C59.zxyw, r1);
  precise vec3 view = C72.xyz - vec3(r0.y, r1.z, r1.x);
  r6.xyz = view;
  bool p0 = r0.x != 0.0;

  if (p0) {
    precise float a = r3.x * r3.x;
    r0.x = a;
    ps = a;
    r0.x = AvFma(r7.x, r7.x, r0.x);
    r0.y = (-abs(r0.x) > C255.y) ? 1.0 : 0.0;
    float inverse_root = inversesqrt(abs(r0.x));
    ps = inverse_root;
    r0.z = inverse_root;
    float x = AvMulX(r3.x, r0.z);
    float z = AvMulX(r7.x, r0.z);
    r0.x = x;
    r0.z = z;
    vec2 cross_a = AvMulX(vec2(r0.x), vec2(-r3.y, -r3.x));
    r1.xy = cross_a;
    ps = r0.z;
    precise float t = AvMulX(r0.z, r7.x) + (-r1.y);
    r1.z = t;
    vec2 negated = vec2(-r1.z, -r1.x);
    float w = AvMulX(-r3.y, ps);
    r1.xy = negated;
    r1.z = w;
  } else {
    precise vec2 scaled = vec2(-r3.x, -r3.y) * C255.z;
    r0.yz = scaled;
    precise float sq = r0.y * r0.y;
    r0.x = sq;
    r0.x = AvFma(-r0.z, -r0.z, r0.x);
    r1.z = (-abs(r0.x) > C255.y) ? 1.0 : 0.0;
    float inverse_root = inversesqrt(abs(r0.x));
    r0.x = inverse_root;
    float a = AvMulX(r0.y, r0.x);
    float old_z = r0.z;
    r1.x = a;
    float b = AvMulX(r1.x, r7.x);
    float c = AvMulX(r0.x, -old_z);
    r0.z = b;
    r1.y = c;
    vec2 d = AvMulX(r1.xy, vec2(-r3.x, -r3.y));
    r0.xy = d;
    precise float e = r0.x - r0.y;
    float f = AvMulX(r7.x, r1.y);
    r0.x = e;
    r0.y = f;
    r0.xy = vec2(-r0.x, -r0.y);
  }

  XeOutputPosition(r2);
  out_uv = vec4(halves.y, halves.x, 1.0, 0.0);
  out_view = vec4(r6.xyz, 0.0);
  out_normal = vec4(r7.xyz, 0.0);
  out_a = vec4(r0.xyz, 0.0);
  out_b = vec4(r1.z, r1.x, r1.y, 0.0);
  out_color = r5;
}
