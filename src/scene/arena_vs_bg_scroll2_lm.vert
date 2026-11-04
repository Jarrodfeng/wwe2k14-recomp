#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena background's two-layer scrolling
// lightmapped material, the game's yBG_UVScrollLM (guest vertex shader 0EB3593F948BBFF0, a vertex-declaration copy of base
// shader 22DA836F618393FB; 3 / 2 draws a frame in Brock's / Bryan's entrance). Pixel side: arena_bg_scroll_blend_lm.frag
// (guest pixel shader DDBEB05B1F79ABF4). Written from our reading of the guest microcode; checked against the translation
// by tools\shaders\harness\vs_harness.py.
//
// The scrolled coordinates as arena_vs_bg_scroll2.vert (here (c203 x c255.y) x c186.x), the lighting map's coordinate as
// arena_vs_bg_lm.vert, lighting with the ambient factor c255.x x facing + c255.x. Interpolators: 0 = layer 1's coordinate
// (xy); 1 = layer 2's (xy); 2 = the lighting map's coordinate (xy); 3 = colour (rgb = vertex colour x light, a = vertex
// alpha x c185.x x c189.w x c190.w).
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 74 76 77 78 180 181 182 184
// 185 186 189 190 203 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[26]; } fc;
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
#define C74   fc.c[12]
#define C76   fc.c[13]
#define C77   fc.c[14]
#define C78   fc.c[15]
#define C180  fc.c[16]
#define C181  fc.c[17]
#define C182  fc.c[18]
#define C184  fc.c[19]
#define C185  fc.c[20]
#define C186  fc.c[21]
#define C189  fc.c[22]
#define C190  fc.c[23]
#define C203  fc.c[24]
#define C255  fc.c[25]
#include "arena_vs.glsl"

layout(location = 0) invariant out vec4 out_uv_a;
layout(location = 1) invariant out vec4 out_uv_b;
layout(location = 2) invariant out vec4 out_light_uv;
layout(location = 3) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index);
  vec4 position = AvFetchPosition(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColor(at);
  vec2 halves = AvFetchHalves(at);

  // The guest's position w is "|r0.x| >= 0" with r0.x the texture coordinate's first half by then.
  vec3 world = AvWorld(position);
  XeOutputPosition(AvClip(world, AvOne(halves.y)));

  precise vec4 scroll = vec4(C203.z, C203.w, C203.x, C203.y) * C255.y;
  vec4 scrolled = AvFma(scroll, vec4(C186.x), vec4(halves.y, halves.x, halves.y, halves.x));
  out_uv_a = vec4(scrolled.z, scrolled.w, 0.0, 0.0);
  out_uv_b = vec4(scrolled.x, scrolled.y, 0.0, 0.0);

  precise vec2 map_scale = C184.xy * C255.x;
  precise vec2 map_product = vec2(world.x * map_scale.x, world.z * map_scale.y);
  precise vec2 light_uv = map_product + C255.x;
  out_light_uv = vec4(light_uv, 0.0, 0.0);

  vec3 nw = AvWorldNormal(normal);
  precise float up_scaled = C255.x * AvUpDot(nw);
  precise float facing = up_scaled + C255.x;
  vec3 lit = AvLit(AvLightTerms(nw), AvAmbient(facing));
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise vec4 shaded = vec4(lit, alpha_scale) * color;
  out_color = shaded;
}
