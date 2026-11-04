#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yFlashDefault (guest vertex shader 1D3A5C6685E70D68;
// 6 / 5 / 0 draws a frame in Brock's entrance / Bryan's entrance / the match). Pixel side: the flash pixel shader 9A39D20F (bench
// census). Hand-written from our reading of the guest shader; checked bit for bit against the SDK's translation by
// tools/shaders/harness/vs_harness.py.
//
// A camera flash (a lit sprite mesh, 8-dword vertex: position, one bone byte at dword 4, a packed normal at 5, a colour at 6, a
// texture coordinate at 7). The bool b0 picks the one bone at c[76 + 3 x byte] (else the plain model rows c76..c78). The flash's centre
// (c214, through the view-projection rows c56..c59) is looked up in the vertex texture of fetch constant 17 (xenos_vs_tex17.glsl) and
// compared with the centre's depth: where the lookup is greater than 1 - depth the vertex's alpha is zero. The lighting is the arena's
// four lights with the directions scaled by the reciprocal length of the turned normal. Interpolators: 0 = (u, v); 1 = the negated unit
// normal; 2 = the camera (c72) less the world position; 3 = zeros; 4 = (flag, flag, flag) with the flag = (-|lit red| > 0); 5 = (flag,
// flag); 6 = the lit colour x vertex colour (alpha: c185.x x c189.w x c190.w x the vertex alpha, zero when the flash is hidden).
// Float constants: the whole block by number (kDynamic).

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#include "xenos_vs_tex17.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_normal;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_zero;
layout(location = 4) invariant out vec4 out_flag;
layout(location = 5) invariant out vec4 out_flag_xy;
layout(location = 6) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 8, 95);
  vec4 lit = fc.c[255];  // 0, 1, 1/2, the bone stride
  vec4 position = AvFetchPosition(at);
  float bone_byte = float(bitfieldExtract(XeFetchDword(at + 4), 0, 8));
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColorAt(at + 6);
  vec2 halves = AvFetchHalves(at);

  // the model (or the one bone) against the position and the normal, fused (summed w, z, x, y and z, x, y)
  int a = 0;
  if (XeBool(0)) {
    precise float scaled = lit.w * trunc(bone_byte);
    a = XeAddress(scaled);
  }
  vec4 q = position.wzxy;
  vec3 world = vec3(AvDot4(q, fc.c[76 + a].wzxy), AvDot4(q, fc.c[77 + a].wzxy), AvDot4(q, fc.c[78 + a].wzxy));
  vec3 n = normal.zxy;
  vec3 turned = vec3(AvDot3(n, fc.c[76 + a].zxy), AvDot3(n, fc.c[77 + a].zxy), AvDot3(n, fc.c[78 + a].zxy));

  // the flash's centre through the view-projection rows, and the lookup at its screen position
  float cx = AvDot4(fc.c[214].wzxy, fc.c[56].wzxy), cy = AvDot4(fc.c[214].wzxy, fc.c[57].wzxy);
  float cz = AvDot4(fc.c[214].wzxy, fc.c[58].wzxy), cw = AvDot4(fc.c[214].wzxy, fc.c[59].wzxy);
  float inv_len = inversesqrt(abs(AvDot3(turned.zxy, turned.zxy)));
  precise float inv_cw = 1.0 / cw;
  precise float half_inv_cw = AvMulX(inv_cw, lit.z);
  float centre_z = AvMulX(cz, inv_cw);
  float centre_x = AvMulX(half_inv_cw, cx), centre_y = AvMulX(half_inv_cw, cy);
  vec3 up_axis = AvMulX3(vec3(-inv_len), fc.c[180].xyz);
  float up = AvDot3R(up_axis.zxy, turned.zxy);
  precise float facing = AvMulX(lit.z, up) + lit.z;
  precise float u = centre_x + lit.z;
  precise float v = centre_y + lit.z;
  precise float v_flipped = lit.y - v;
  float visible_from_texture = XeVertexTexture17(vec2(u, v_flipped)).x;
  precise float depth_left = lit.y - centre_z;
  bool hidden = (visible_from_texture > depth_left);

  // the clip position
  vec4 p = vec4((lit.x == 0.0) ? world.z : lit.y, (lit.x == 0.0) ? world.x : lit.y, (lit.x == 0.0) ? world.y : lit.y,
                (lit.y == 0.0) ? world.y : lit.y);
  vec4 clip = vec4(AvDot4(fc.c[56].zxyw, p), AvDot4(fc.c[57].zxyw, p), AvDot4(fc.c[58].zxyw, p), AvDot4(fc.c[59].zxyw, p));
  XeOutputPosition(clip);

  // the four lights: each direction scaled by the reciprocal length of the turned normal (both negated), against the normal
  vec3 s8 = AvMulX3(vec3(-inv_len), -fc.c[8].xyz), s9 = AvMulX3(vec3(-inv_len), -fc.c[9].xyz);
  vec3 s10 = AvMulX3(vec3(-inv_len), -fc.c[10].xyz), s11 = AvMulX3(vec3(-inv_len), -fc.c[11].xyz);
  float d8 = AvDot3R(s8.zxy, turned.zxy), d9 = AvDot3R(s9.zxy, turned.zxy);
  float d10 = AvDot3R(s10.zxy, turned.zxy), d11 = AvDot3R(s11.zxy, turned.zxy);
  float t8 = (d8 >= fc.c[74].x) ? d8 : fc.c[74].x;
  float t11 = (d11 >= fc.c[74].w) ? d11 : fc.c[74].w;
  float t10 = (d10 >= fc.c[74].z) ? d10 : fc.c[74].z;
  float t9 = (d9 >= fc.c[74].y) ? d9 : fc.c[74].y;
  precise vec3 sum = AvMulX3(vec3(t11), fc.c[3].xyz);
  sum = AvMulX3(vec3(t10), fc.c[2].xyz) + sum;
  sum = AvMulX3(vec3(t9), fc.c[1].xyz) + sum;
  sum = AvMulX3(vec3(t8), fc.c[0].xyz) + sum;
  precise vec3 span = fc.c[182].xyz + (-fc.c[181].xyz);
  precise vec3 blended = AvMulX3(span, vec3(facing)) + fc.c[181].xyz;
  precise vec3 ambient = AvMulX3(blended, fc.c[189].xyz);
  precise vec3 lit_color = AvMulX3(sum, fc.c[190].xyz) + ambient;
  precise float alpha_base = fc.c[190].w * fc.c[189].w;
  precise float alpha_scale = fc.c[185].x * alpha_base;
  vec3 rgb = AvMulX3(lit_color, color.rgb);
  float alpha = hidden ? 0.0 : AvMulX(alpha_scale, color.w);
  float flag = ((-abs(rgb.x)) > lit.x) ? 1.0 : 0.0;

  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  out_normal = vec4(AvMulX3(vec3(-inv_len), turned), 0.0);
  precise vec3 view = (-world) + fc.c[72].xyz;
  out_view = vec4(view, 0.0);
  out_zero = vec4(0.0);
  out_flag = vec4(flag, flag, flag, 0.0);
  out_flag_xy = vec4(flag, flag, 0.0, 0.0);
  out_color = vec4(rgb, alpha);
}
