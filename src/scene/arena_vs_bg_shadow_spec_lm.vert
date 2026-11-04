#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena background's shadow-receiving highlighted
// lightmapped material (the entrance stage's floor), the game's yBG_DefaultLM (guest vertex shader 5C0694B2FE1B593A, a
// vertex-declaration copy of base shader 0159FD1A4AE34E32; 12 draws in Brock's entrance). Pixel side:
// arena_bg_shadow_spec_mask_lm.frag (guest pixel shader 9B46FDF960C4CC1F). Written from our reading of the guest
// microcode; checked against the translation by tools\shaders\harness\vs_harness.py.
//
// arena_vs_bg_shadow_lm.vert plus the highlight's normal and view vector (arena_vs_bg_spec_lm.vert). Interpolators:
// 0 = (u, v, 1); 1 = the negated world normal; 2 = camera (c72) less world position; 3 = the shadow map's projected
// coordinate; 4 = (cmp, cmp, c169.y x (c255.y - max(height, c255.z)), 0) with cmp = (-|height| > c255.z), height = the
// normal against c166; 5 = lighting map coordinate; 6 = colour (rgb = vertex colour x light, a = vertex alpha x c185.x x
// c189.w x c190.w).
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 68 69 71 72 74 76 77 78 166
// 169 180 181 182 184 185 189 190 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[30]; } fc;
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
#define C72   fc.c[15]
#define C74   fc.c[16]
#define C76   fc.c[17]
#define C77   fc.c[18]
#define C78   fc.c[19]
#define C166  fc.c[20]
#define C169  fc.c[21]
#define C180  fc.c[22]
#define C181  fc.c[23]
#define C182  fc.c[24]
#define C184  fc.c[25]
#define C185  fc.c[26]
#define C189  fc.c[27]
#define C190  fc.c[28]
#define C255  fc.c[29]
#include "arena_vs.glsl"
#include "arena_vs_shadow.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_normal;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_shadow;
layout(location = 4) invariant out vec4 out_height;
layout(location = 5) invariant out vec4 out_light_uv;
layout(location = 6) invariant out vec4 out_color;

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
  out_uv = vec4(halves.y, halves.x, 1.0, 0.0);

  vec3 nw = AvWorldNormal(normal);
  float up = AvUpDot(nw);

  // The lighting map's coordinate: world x (c76) and z (c78) times the map's scale c184.xy and c255.x, plus c255.x.
  precise vec2 map_scale = C184.xy * C255.x;
  precise vec2 map_product = vec2(world.x * map_scale.x, world.z * map_scale.y);
  precise vec2 light_uv = map_product + C255.x;
  out_light_uv = vec4(light_uv, 0.0, 0.0);
  out_normal = vec4(-nw, 0.0);
  precise vec3 view = (-world) + C72.xyz;
  out_view = vec4(view, 0.0);
  out_shadow = AvShadowCoord(world, one);
  float height = AvShadowHeight(nw);
  float cmp = (-abs(height) > C255.z) ? 1.0 : 0.0;
  float floored = height >= C255.z ? height : C255.z;
  precise float rest = C255.y - floored;
  precise float weight = C169.y * rest;
  out_height = vec4(cmp, cmp, weight, 0.0);

  precise float up_scaled = C255.x * up;
  precise float facing = up_scaled + C255.x;
  vec3 lit = AvLit(AvLightTerms(nw), AvAmbient(facing));
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise vec4 shaded = vec4(lit, alpha_scale) * color;
  out_color = shaded;
}
