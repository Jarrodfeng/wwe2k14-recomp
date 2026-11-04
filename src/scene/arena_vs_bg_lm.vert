#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena background's lightmapped material, the
// game's yBG_DefaultLM - the most drawn vertex shader of the bench (143 / 101 draws a frame in Brock's / Bryan's entrance;
// guest vertex shader 4F1118D4B3F6D388, a vertex-declaration copy of base shader 47B4C1C644E28234) - in place of the
// translation (src\scene_vs_swap.h). Pixel side: arena_bg_lm.frag (guest pixel shader 2BC8853B576D423E), whose inputs it
// feeds. Written from our reading of the guest microcode; checked against the translation on random vertices and constants
// by tools\shaders\harness\vs_harness.py.
//
// Per vertex: the position through the model rows c76 / c77 / c78 and the view-projection rows c56..c59; the vertex colour
// times the arena's lighting (arena_vs.glsl: ambient c181 / c182 by the normal against c180, four lights c0..c3 / c8..c11
// floored at c74, times c190 plus c189 times the ambient); the colour texture's coordinate; and the lighting map's
// coordinate - a top-down map: the world position's first and third terms (c76 and c78 rows) times the map's scale c184.xy
// times c255.x, plus c255.x (the compiler's literal, 0.5 by the crowd shaders' use of the same maths, unconfirmed here).
// Interpolators (what the pixel shader reads in bold): 0 = texture coordinate (xy); 1 = zeros; 2 = (-|u| > c255.y) in xyz,
// a compare the compiler uses for a constant (0 for c255.y >= 0; kept as the guest computes it); 3 = lighting map
// coordinate (xy); 4 = colour: rgb = vertex colour x light, a = vertex alpha x (c185.x x c189.w x c190.w).
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 74 76 77 78 180 181 182 184
// 185 189 190 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[24]; } fc;
#define C0   fc.c[0]
#define C1   fc.c[1]
#define C2   fc.c[2]
#define C3   fc.c[3]
#define C8   fc.c[4]
#define C9   fc.c[5]
#define C10  fc.c[6]
#define C11  fc.c[7]
#define C56  fc.c[8]
#define C57  fc.c[9]
#define C58  fc.c[10]
#define C59  fc.c[11]
#define C74  fc.c[12]
#define C76  fc.c[13]
#define C77  fc.c[14]
#define C78  fc.c[15]
#define C180 fc.c[16]
#define C181 fc.c[17]
#define C182 fc.c[18]
#define C184 fc.c[19]
#define C185 fc.c[20]
#define C189 fc.c[21]
#define C190 fc.c[22]
#define C255 fc.c[23]
#include "arena_vs.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_one;
layout(location = 2) invariant out vec4 out_two;
layout(location = 3) invariant out vec4 out_light_uv;
layout(location = 4) invariant out vec4 out_color;

void main() {
  int at = XeVertexAddress(XeGuestVertexIndex());
  vec4 position = AvFetchPosition(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColor(at);
  vec2 halves = AvFetchHalves(at);

  // The guest's position w comes from "|r0.x| >= 0", r0.x holding the texture coordinate's first half by then.
  vec3 world = AvWorld(position);
  XeOutputPosition(AvClip(world, AvOne(halves.y)));

  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  out_one = vec4(0.0);
  float two = (-abs(halves.y) > C255.y) ? 1.0 : 0.0;
  out_two = vec4(two, two, two, 0.0);

  // The lighting map's coordinate: world x (c76) and world z (c78) times the map's scale and c255.x, plus c255.x.
  precise vec2 map_scale = C184.xy * C255.x;
  precise vec2 map_uv = vec2(world.x * map_scale.x, world.z * map_scale.y);
  precise vec2 light_uv = map_uv + C255.x;
  out_light_uv = vec4(light_uv, 0.0, 0.0);

  // Lighting: the ambient blend factor is c255.x times how far the normal faces c180, plus c255.x.
  vec3 nw = AvWorldNormal(normal);
  precise float up_scaled = C255.x * AvUpDot(nw);
  precise float facing = up_scaled + C255.x;
  vec3 lit = AvLit(AvLightTerms(nw), AvAmbient(facing));
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise vec4 shaded = vec4(lit, alpha_scale) * color;
  out_color = shaded;
}
