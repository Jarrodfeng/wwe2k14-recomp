#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the wrestlers' plane shadows, the game's yPlaneShadowMap
// (guest vertex shader A96C8C4D28D28F80, a vertex-declaration copy of base shader 92A1C148...; 8 / 8 / 24 draws a frame in Brock's entrance
// / Bryan's entrance / the match). Pixel side: debug_primitive.frag (guest pixel shader A4A965C189287B99). Written from our
// reading of the guest microcode; checked against the translation by tools\\shaders\\harness\\vs_harness.py.
//
// A skinned vertex (position, bone bytes and three weights (dwords 4-6)): the bool constants b4 / b3 / b2 / b1 choose the bone count (matrices at
// c[76 + 3 x byte]; the whole float block is indexed - kDynamic). The skinned world position is projected from the light
// c72 onto the plane at height c7.y, through the shadow rows c13..c16 and c9 / c10 / c12; z and the interpolator carry the
// fade (a sum of power curves of the distance with c1..c6 and c0.x). Interpolator 0 = (fade, 0, 0, 1).
// Float constants (set 1 binding 1): all 256 registers by number.

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"

layout(location = 0) invariant out vec4 out_a;

#define C0  fc.c[0]
#define C1  fc.c[1]
#define C2  fc.c[2]
#define C3  fc.c[3]
#define C4  fc.c[4]
#define C5  fc.c[5]
#define C6  fc.c[6]
#define C7  fc.c[7]
#define C9  fc.c[9]
#define C10 fc.c[10]
#define C12 fc.c[12]
#define C13 fc.c[13]
#define C14 fc.c[14]
#define C15 fc.c[15]
#define C16 fc.c[16]
#define C72 fc.c[72]

float Dp2Add(vec2 a, vec2 b, float c) {
  precise float s = AvMulX(a.x, b.x) + AvMulX(a.y, b.y);
  precise float t = s + c;
  return t;
}

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 11, 95);
  vec4 r3 = vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), XeFetchFloat(at + 3));
  vec4 r1 = vec4(XeFetchFloat(at + 4), XeFetchFloat(at + 6), XeFetchFloat(at + 5), 1.0);
  uint packed_bytes = XeFetchDword(at + 7);
  vec4 r2 = vec4(bitfieldExtract(packed_bytes, 8, 8), bitfieldExtract(packed_bytes, 16, 8), bitfieldExtract(packed_bytes, 24, 8), bitfieldExtract(packed_bytes, 0, 8));
  vec4 r0 = vec4(0.0), r4 = vec4(0.0), r5 = vec4(0.0), r6 = vec4(0.0);
  // the bytes (the fetch's yzwx swizzle): r2.x = byte 1, r2.y = byte 2, r2.z = byte 3, r2.w = byte 0
  r0.y = trunc(r2.w);
  precise float k2 = AvMulX(C255.x, r0.y);
  r2.w = k2;
  if (XeBool(4)) {
    precise float s01 = r1.x + r1.z;
    r0.y = s01;
    r0.z = r2.y;
    precise float s012 = r0.y + r1.y;
    r5.y = s012;
    r0.y = r2.x;
    r0.w = r2.z;
    r0 = vec4(trunc(r0.y), trunc(r0.z), trunc(r0.w), trunc(r0.x));
    int a = XeAddress(r2.w);
    r5.x = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r5.z = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r6.z = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r4 = vec4(AvMulX(r0.y, C255.x), AvMulX(r0.x, C255.x), AvMulX(r0.w, C255.x), AvMulX(r0.z, C255.x));
    precise float t1 = r5.y + r1.w;
    r0.x = t1;
    a = XeAddress(r4.z);
    r2.x = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r2.y = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r2.z = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    a = XeAddress(r4.x);
    r4.x = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r4.z = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r6.x = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    a = XeAddress(r4.y);
    r5.y = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r5.w = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r6.y = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    a = XeAddress(r4.w);
    r4.y = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r4.w = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r6.w = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r0.y = AvDot4R(r6.wxzy, r1.wyxz);
    r3 = vec4(AvMulX(r5.x, r1.x), AvMulX(r5.y, r1.z), AvMulX(r5.z, r1.x), AvMulX(r5.w, r1.z));
    precise vec2 pair = vec2(r3.z + r3.w, r3.x + r3.y);
    r0.zw = pair;
    precise float comp = C255.y - r0.x;
    r0.x = comp;
    r0.z = Dp2Add(r4.zw, r1.yw, r0.z);
    r0.w = Dp2Add(r4.xy, r1.yw, r0.w);
    precise vec3 mixed = AvMulX3(r2.zxy, vec3(r0.x)) + vec3(r0.z, r0.w, r0.y);
    r0.xyz = mixed;
  } else if (XeBool(3)) {
    precise float s01 = r1.x + r1.z;
    precise float s012 = s01 + r1.y;
    r0.x = s012;
    r0.y = trunc(r2.x);
    r0.z = trunc(r2.y);
    r0.w = trunc(r2.z);
    int a = XeAddress(r2.w);
    r4.x = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r4.z = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    vec3 scaled = AvMulX3(vec3(r0.y, r0.w, r0.z), vec3(C255.x));
    r0.y = scaled.x;
    r0.z = scaled.y;
    r0.w = scaled.z;
    r4.w = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    a = XeAddress(r0.z);
    r2.x = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r2.y = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r2.z = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    a = XeAddress(r0.y);
    r4.y = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r5.x = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r5.z = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r0.y = AvMulX(r4.z, r1.x);
    r0.z = AvMulX(r4.w, r1.x);
    a = XeAddress(r0.w);
    r4.z = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r5.y = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r5.w = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    float z2 = Dp2Add(r5.zw, r1.zy, r0.z);
    float w2 = Dp2Add(r5.xy, r1.zy, r0.y);
    r0.z = z2;
    r0.w = w2;
    r0.y = AvDot3R(r4.zxy, r1.yxz);
    precise float comp = C255.y - r0.x;
    r0.x = comp;
    precise vec3 mixed = AvMulX3(r2.zxy, vec3(r0.x)) + vec3(r0.z, r0.w, r0.y);
    r0.xyz = mixed;
  } else if (XeBool(2)) {
    precise float s01 = r1.x + r1.z;
    r0.x = s01;
    int a = XeAddress(r2.w);
    r4.x = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r0.y = trunc(r2.x);
    r4.y = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r0.z = trunc(r2.y);
    r0.y = AvMulX(r0.y, C255.x);
    r0.z = AvMulX(r0.z, C255.x);
    precise float comp = C255.y - r0.x;
    r0.x = comp;
    r4.z = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    a = XeAddress(r0.y);
    r2.x = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r2.y = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r2.z = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    a = XeAddress(r0.z);
    r0.y = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r0.z = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r0.w = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
    vec3 first = AvMulX3(vec3(r0.y, r0.z, r0.w), vec3(r0.x));
    precise vec3 m1 = AvMulX3(vec3(r2.z, r2.y, r2.x), vec3(r1.z)) + vec3(first.y, first.z, first.x);
    precise vec3 m2 = AvMulX3(vec3(r4.y, r4.x, r4.z), vec3(r1.x)) + vec3(m1.y, m1.z, m1.x);
    r0.xyz = m2;
  } else {
    if (XeBool(1)) {
      r0.y = trunc(r2.x);
      precise float comp = C255.y - r1.x;
      r0.x = comp;
      r0.y = AvMulX(r0.y, C255.x);
      int a = XeAddress(r2.w);
      r1.y = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
      r1.z = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
      r1.w = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
      a = XeAddress(r0.y);
      r0.y = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
      r0.z = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
      r0.w = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
      vec3 first = AvMulX3(r0.yzw, vec3(r0.x));
      precise vec3 m = AvMulX3(vec3(r1.z, r1.y, r1.w), vec3(r1.x)) + vec3(first.x, first.y, first.z).yxz;
      r0.xyz = m;
    } else {
      int a = XeAddress(r2.w);
      r0.y = AvDot4R(r3.wzxy, fc.c[76 + XeBone(a)].wzxy);
      r0.x = AvDot4R(r3.wzxy, fc.c[77 + XeBone(a)].wzxy);
      r0.z = AvDot4R(r3.wzxy, fc.c[78 + XeBone(a)].wzxy);
    }
  }
  // the plane shadow: the world position projected from the light c72 onto the plane at c7.y, then the shadow rows
  precise float plane = C7.y + (-C72.y);
  r1.w = plane;
  precise vec3 toward = r0.xyz + (-C72.yxz);
  r1.xyz = toward;
  precise vec3 away = (-vec3(r0.y, r0.x, r0.z)) + C72.xyz;
  float inv = 1.0 / r1.x;
  r0.xyz = away;
  r0.w = inv;
  float ratio = AvMulX(r1.w, r0.w);
  r0.w = ratio;
  precise vec3 hit = AvMulX3(vec3(r0.w), r1.yxz) + C72.xyz;
  r2.xyz = hit;
  precise vec3 hit2 = AvMulX3(vec3(r0.w), r1.yxz) + r0.xyz;
  float len2 = AvDot3(hit2.zxy, hit2.zxy);
  float one = AvOne(hit2.x);
  r0.xyz = hit2;
  r2.w = one;
  r1.z = AvDot4R(C15.zxyw, r2.zxyw);
  r1.w = AvDot4R(C16.zxyw, r2.zxyw);
  r1.x = AvDot4R(C13.zxyw, r2.zxyw);
  r1.y = AvDot4R(C14.zxyw, r2.zxyw);
  float inv_scale = 1.0 / C0.x;
  float dist = sqrt(abs(len2));
  float fade = AvMulX(inv_scale, dist);
  fade = isnan(fade) ? 0.0 : clamp(fade, 0.0, 1.0);
  precise float rest = C255.y - fade;
  float lg = log2(abs(rest));
  vec4 clip;
  clip.x = AvDot4R(r1.wzxy, C9.wzxy);
  clip.y = AvDot4R(r1.wzxy, C10.wzxy);
  clip.w = AvDot4R(r1.wzxy, C12.wzxy);
  float e0 = AvMulX(C3.x, lg);
  float e1 = AvMulX(lg, C1.x);
  float p0 = exp2(e0);
  float e2 = AvMulX(lg, C2.x);
  float p1 = exp2(e1);
  float zw = AvDot4R(r1.wzxy, C12.wzxy);
  float t4 = AvMulX(C4.x, p1);
  float t6 = AvMulX(p0, C6.x);
  float p2 = exp2(e2);
  precise float comb = AvMulX(p2, C5.x) + t6;
  precise float sum = comb + t4;
  float sat = isnan(sum) ? 0.0 : clamp(sum, 0.0, 1.0);
  precise float lastz = C255.y - sat;
  clip.z = AvMulX(lastz, zw);
  XeOutputPosition(clip);
  out_a = vec4(comb + t4, 0.0, 0.0, 1.0);
}
