#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
#include "xenos_vs_tex.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena background's highlighted material with a
// vertex-side lighting lookup, the game's yBG_Default (guest vertex shader 94FE8F169F50CF85, a vertex-declaration copy of
// base shader 68815C03D7B4FFB2; 1 draw in the match). Pixel side: arena_bg_spec.frag (guest pixel shader
// 775AF960824D6ADC). Written from our reading of the guest microcode; checked against the translation by
// tools\shaders\harness\vs_harness.py.
//
// arena_vs_bg_spec_lm_tf.vert without the lighting map's coordinate output (it only addresses the vertex fetch) and with the
// compare reading the map coordinate's x product. Interpolators: 0 = (u, v, 1); 1 = negated world normal; 2 = camera less
// world position; 3 = zeros; 4 = (cmp, cmp, cmp, 0) with cmp = (-|c255.x x c184.x x world x| > c255.y); 5 = colour.
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 72 74 76 77 78 180 181 182
// 184 185 189 190 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[25]; } fc;
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
#define C72   fc.c[12]
#define C74   fc.c[13]
#define C76   fc.c[14]
#define C77   fc.c[15]
#define C78   fc.c[16]
#define C180  fc.c[17]
#define C181  fc.c[18]
#define C182  fc.c[19]
#define C184  fc.c[20]
#define C185  fc.c[21]
#define C189  fc.c[22]
#define C190  fc.c[23]
#define C255  fc.c[24]
#include "arena_vs.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_normal;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_zero;
layout(location = 4) invariant out vec4 out_flag;
layout(location = 5) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index);
  vec4 position = AvFetchPosition(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColor(at);
  vec2 halves = AvFetchHalves(at);

  vec3 world = AvWorld(position);
  XeOutputPosition(AvClip(world, AvOne(float(index))));
  out_uv = vec4(halves.y, halves.x, 1.0, 0.0);

  vec3 nw = AvWorldNormal(normal);
  float up = AvUpDot(nw);

  // The lighting map's coordinate: world x (c76) and z (c78) times the map's scale c184.xy and c255.x, plus c255.x.
  precise float map_x_scale = C255.x * C184.x;
  precise float map_y_scale = C255.x * C184.y;
  precise vec2 map_product = vec2(map_x_scale * world.x, map_y_scale * world.z);
  precise vec2 light_uv = map_product + C255.x;
  out_normal = vec4(-nw, 0.0);
  precise vec3 view = (-world) + C72.xyz;
  out_view = vec4(view, 0.0);
  out_zero = vec4(0.0);
  float cmp = (-abs(map_product.x) > C255.y) ? 1.0 : 0.0;
  out_flag = vec4(cmp, cmp, cmp, 0.0);

  precise float up_scaled = C255.x * up;
  precise float facing = up_scaled + C255.x;
  vec3 lit = AvLit(AvLightTerms(nw), AvAmbient(facing));

  // The lighting map read here: vertex colour x map x c184.z, times the light.
  vec3 map = XeVertexTexture16(light_uv);
  precise vec3 map_scaled = map * C184.z;
  precise vec3 vertex_light = map_scaled * color.rgb;
  precise vec3 rgb = vertex_light * lit;
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise float alpha = color.w * alpha_scale;
  out_color = vec4(rgb, alpha);
}
