#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the props' and rigid parts' shadow maps, the game's yShadowMap (guest vertex shader BDAF8155EBF110C2, a
// vertex-declaration copy of base shader 9DAC5F03...; 22 / 22 / 38 draws a frame in Brock's entrance / Bryan's entrance / the
// match). Pixel side: shadow_map.frag (guest pixel shader DA494C9383E18961). Written from our reading of the guest microcode; checked against the translation by
// tools\\shaders\\harness\\vs_harness.py.
//
// A skinned vertex: a position (4 floats) and four bone bytes (no weights: a rigid vertex). The bone matrices are at c[76 + 3 x byte] (three rows each;
// the whole 256-register block is indexed - the registration flag kDynamic - and the bool constants b3 / b2 / b1 choose four,
// three, two or one bone). The skinned world position goes through the view-projection rows c56..c59 and then the shadow map's c64..c67; interpolator 0 = c54 against the world position (the depth for the map's fade). All multiplies keep the guest's rule (the constants are indexed, so none is proven finite).
// Float constants (set 1 binding 1): all 256 registers by number.

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"

layout(location = 0) invariant out vec4 out_depth;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 8, 95);
  vec4 r0 = vec4(float(index), 0.0, 0.0, 0.0);
  vec4 r2 = vec4(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2), XeFetchFloat(at + 3));
  uint packed_bytes = XeFetchDword(at + 4);
  vec4 r1 = vec4(bitfieldExtract(packed_bytes, 0, 8), bitfieldExtract(packed_bytes, 8, 8), bitfieldExtract(packed_bytes, 16, 8),
                 bitfieldExtract(packed_bytes, 24, 8));
  vec4 r3 = vec4(0.0), r4 = vec4(0.0), r5 = vec4(0.0);
  float ps;

  r1.yzw = trunc(r1.yzw);
  r0.x = trunc(r1.x);
  r1.x = AvMulX(C255.z, r0.x);

  if (XeBool(3)) {
    // four bones
    precise float weights_zw = r0.z + r0.w;
    r0.x = weights_zw;
    r3.xyz = AvMulX3(r1.yzw, vec3(C255.z));
    int a = XeAddress(r1.x);
    r4.x = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r5.x = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r5.y = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
    a = XeAddress(r3.z);
    r1.y = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r1.z = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r1.w = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
    a = XeAddress(r3.x);
    r4.y = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r3.x = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r3.z = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
    a = XeAddress(r3.y);
    r4.z = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r3.y = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r5.z = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r2.xyz = AvMulX3(vec3(r5.z, r5.y, r5.x), vec3(r0.y, r0.z, r0.z));
    ps = r0.x;
    r3.xyz = AvMulX3(vec3(r3.z, r3.x, r3.y), vec3(r0.w, r0.w, r0.y));
    precise float weight_sum = r0.y + ps;
    r0.x = weight_sum;
    precise vec2 blended = r2.yz + r3.xy;
    r2.yz = blended;
    precise float first_weight = C255.y - r0.x;
    r1.x = first_weight;
    float dotted = AvDot3R(vec3(r4.z, r4.y, r4.x), vec3(r0.y, r0.w, r0.z));
    ps = r2.y;
    r0.z = dotted;
    precise float sum_y = r2.z + r3.z;
    precise float sum_x = r2.x + ps;
    r0.y = sum_y;
    r0.x = sum_x;
    precise vec3 skinned = AvMulX3(vec3(r1.y, r1.w, r1.z), vec3(r1.x)) + vec3(r0.z, r0.y, r0.x);
    r0.xyz = skinned;
  } else if (XeBool(2)) {
    // three bones
    precise float weights_zw = r0.z + r0.w;
    precise float rest = C255.y - weights_zw;
    r0.x = rest;
    r3.x = AvMulX(r1.y, C255.z);
    r3.w = AvMulX(r1.z, C255.z);
    int a = XeAddress(r1.x);
    r1.x = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r1.y = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r1.z = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
    a = XeAddress(r3.x);
    r3.x = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r3.y = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r3.z = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
    a = XeAddress(r3.w);
    r4.x = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
    r4.y = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
    r4.z = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
    r2.xyz = AvMulX3(r4.xyz, vec3(r0.x));
    float w = r0.w;
    precise vec3 mid = AvMulX3(vec3(r3.z, r3.y, r3.x), vec3(w)) + vec3(r2.y, r2.z, r2.x);
    r0.x = mid.x;
    r0.y = mid.y;
    r0.w = mid.z;
    precise vec3 skinned = AvMulX3(vec3(r1.y, r1.x, r1.z), vec3(r0.z)) + vec3(r0.w, r0.y, r0.x);
    r0.xyz = skinned;
  } else {
    // two bones or one
    if (XeBool(1)) {
      precise float rest = C255.y - r0.z;
      r0.x = rest;
      r0.y = AvMulX(r1.y, C255.z);
      int a = XeAddress(r1.x);
      r1.x = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
      r1.y = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
      r1.z = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
      a = XeAddress(r0.y);
      r3.x = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
      r3.y = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
      r3.z = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
      vec3 part = AvMulX3(r3.xyz, vec3(r0.x));
      r0.x = part.x;
      r0.y = part.y;
      r0.w = part.z;
      precise vec3 skinned = AvMulX3(r1.xyz, vec3(r0.z)) + vec3(r0.x, r0.y, r0.w);
      r0.xyz = skinned;
    } else {
      int a = XeAddress(r1.x);
      r0.x = AvDot4R(r2.wzxy, fc.c[76 + XeBone(a)].wzxy);
      r0.y = AvDot4R(r2.wzxy, fc.c[77 + XeBone(a)].wzxy);
      r0.z = AvDot4R(r2.wzxy, fc.c[78 + XeBone(a)].wzxy);
    }
  }
  r0 = vec4((C255.x == 0.0) ? r0.z : C255.y, (C255.x == 0.0) ? r0.x : C255.y, (C255.x == 0.0) ? r0.y : C255.y,
            (C255.y == 0.0) ? r0.y : C255.y);
  r1.x = AvDot4R(C56.zxyw, r0);
  r1.y = AvDot4R(C57.zxyw, r0);
  r1.z = AvDot4R(C58.zxyw, r0);
  r1.w = AvDot4R(C59.zxyw, r0);
  XeOutputPosition(vec4(AvDot4R(r1.wzxy, C64.wzxy), AvDot4R(r1.wzxy, C65.wzxy), AvDot4R(r1.wzxy, C66.wzxy),
                        AvDot4R(r1.wzxy, C67.wzxy)));
  out_depth = vec4(AvDot4R(C54.zxyw, r0), 0.0, 0.0, 0.0);
}
