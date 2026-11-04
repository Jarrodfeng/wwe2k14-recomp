#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena background's shadow-receiving plain
// material (the floor), the game's yBG_Default (guest vertex shader E50620DCD7526CF0, a vertex-declaration copy of base
// shader 1D250AA84A721D12; 2 draws in Brock's entrance, 2 in the match). Pixel side: arena_bg_shadow_plain.frag (guest pixel
// shader B63CD5FCA34C147D). Written from our reading of the guest microcode; checked against the translation by
// tools\shaders\harness\vs_harness.py.
//
// As arena_vs_bg.vert, plus the shadow's coordinate (arena_vs_shadow.glsl) and the weight of the shadow terms. Interpolators:
// 0 = texture coordinate (xy); 1 = the shadow map's projected coordinate (rows c68, c69, c166, c71; x y w, z the ramp's
// input); 2 = (cmp, cmp, (c255.z - max(normal against c166, c255.x)) x c169.y, 0) with cmp = (-|normal against c180| >
// c255.x); 3 = colour (rgb = vertex colour x light, a = vertex alpha x c185.x x c189.w x c190.w).
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 68 69 71 74 76 77 78 166 169
// 180 181 182 185 189 190 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[28]; } fc;
#define C0    fc.c[0]
#define C1    fc.c[1]
#define C2    fc.c[2]
#define C3    fc.c[3]
#define C8    fc.c[4]
#define C9    fc.c[5]
#define C10   fc.c[6]
#define C11   fc.c[7]
#define C56   fc.c[8]
#define C57   fc.c[9]
#define C58   fc.c[10]
#define C59   fc.c[11]
#define C68   fc.c[12]
#define C69   fc.c[13]
#define C71   fc.c[14]
#define C74   fc.c[15]
#define C76   fc.c[16]
#define C77   fc.c[17]
#define C78   fc.c[18]
#define C166  fc.c[19]
#define C169  fc.c[20]
#define C180  fc.c[21]
#define C181  fc.c[22]
#define C182  fc.c[23]
#define C185  fc.c[24]
#define C189  fc.c[25]
#define C190  fc.c[26]
#define C255  fc.c[27]
#include "arena_vs.glsl"
#include "arena_vs_shadow.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_shadow;
layout(location = 2) invariant out vec4 out_amount;
layout(location = 3) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index);
  vec4 position = AvFetchPosition(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColor(at);
  vec2 halves = AvFetchHalves(at);

  vec3 world = AvWorld(position);
  float one = AvOne(float(index));
  XeOutputPosition(AvClip(world, one));

  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  vec3 nw = AvWorldNormal(normal);
  float up = AvUpDot(nw);
  out_shadow = AvShadowCoord(world, one);
  float cmp = (-abs(up) > C255.x) ? 1.0 : 0.0;
  precise float weight = AvShadowAmount(AvShadowHeight(nw)) * C169.y;
  out_amount = vec4(cmp, cmp, weight, 0.0);

  float facing = AvFma(up, C255.y, C255.y);
  vec3 lit = AvLit(AvLightTerms(nw), AvAmbient(facing));
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise vec4 shaded = vec4(lit, alpha_scale) * color;
  out_color = shaded;
}
