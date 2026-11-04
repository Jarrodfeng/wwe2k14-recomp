#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena background's highlighted material, the
// game's yBG_Default with a specular highlight (guest vertex shader 2A3E3B9E6EDE114E, a vertex-declaration copy of base
// shader E9B7B08761832F42; 4 / 2 / 2 draws a frame in Brock's / Bryan's entrance / the match). Pixel side:
// arena_bg_spec.frag and arena_bg_spec_plain.frag (guest pixel shaders 775AF960824D6ADC, 748BF8B0868777D3). Written from our
// reading of the guest microcode; checked against the translation by tools\shaders\harness\vs_harness.py.
//
// As arena_vs_bg.vert, plus what the highlight needs: the surface normal (the world normal negated, in the guest's axis
// order) and the view vector (c72, the camera, less the world position). Interpolators: 0 = (u, v, 1) - the third the
// highlight's scale; 1 = the negated world normal (xyz); 2 = camera less world position (xyz); 3 = (cmp, 0, 0, 0) and 4 =
// (cmp, cmp, cmp, 0) with cmp = (-|normal against c180| > c255.x) (compiler constants); 5 = colour (rgb = vertex colour x
// light, a = vertex alpha x c185.x x c189.w x c190.w).
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 72 74 76 77 78 180 181 182
// 185 189 190 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[24]; } fc;
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
#define C185  fc.c[20]
#define C189  fc.c[21]
#define C190  fc.c[22]
#define C255  fc.c[23]
#include "arena_vs.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_normal;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_flag_a;
layout(location = 4) invariant out vec4 out_flag_b;
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

  vec3 nw = AvWorldNormal(normal);
  float up = AvUpDot(nw);
  out_uv = vec4(halves.y, halves.x, 1.0, 0.0);
  out_normal = vec4(-nw, 0.0);
  precise vec3 view = (-world) + C72.xyz;
  out_view = vec4(view, 0.0);
  float cmp = (-abs(up) > C255.x) ? 1.0 : 0.0;
  out_flag_a = vec4(cmp, 0.0, 0.0, 0.0);
  out_flag_b = vec4(cmp, cmp, cmp, 0.0);

  float facing = AvFma(up, C255.y, C255.y);
  vec3 lit = AvLit(AvLightTerms(nw), AvAmbient(facing));
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise vec4 shaded = vec4(lit, alpha_scale) * color;
  out_color = shaded;
}
