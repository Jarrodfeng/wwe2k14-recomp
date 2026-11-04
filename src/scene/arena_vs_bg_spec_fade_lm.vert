#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena background's highlighted lightmapped
// material with a faded highlight, the game's yBG_DefaultLM (guest vertex shader 2F1B8D1655D529F5, a vertex-declaration
// copy of base shader 046E90BF5AF35185; 8 draws in Brock's entrance). Pixel side: arena_bg_spec_mask_lm.frag (guest pixel
// shader AA33C0D66574EE32). Written from our reading of the guest microcode; checked against the translation by
// tools\shaders\harness\vs_harness.py.
//
// arena_vs_bg_spec_lm.vert with the highlight's scale faded by depth as arena_vs_bg_spec_fade.vert does (here c255.y less
// the ramp), and the compare reading the ramp's input (clip z less c209.x) against c255.z. Interpolators: 0 = (u, v,
// scale); 1 = the negated world normal; 2 = camera less world position; 3 = (cmp, 0, 0, 0); 4 = (cmp, cmp, cmp, 0);
// 5 = lighting map coordinate; 6 = colour.
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 72 74 76 77 78 180 181 182
// 184 185 189 190 209 255.

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
#define C209  fc.c[24]
#define C255  fc.c[25]
#include "arena_vs.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_normal;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_flag_a;
layout(location = 4) invariant out vec4 out_flag_b;
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
  vec4 clip = AvClip(world, AvOne(float(index)));
  XeOutputPosition(clip);

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

  // The depth ramp: (clip z - c209.x) / (c209.y - c209.x), saturated (a NaN to 0).
  precise float ramp_span = C209.y - C209.x;
  precise float ramp_scale = 1.0 / ramp_span;
  precise float ramp_in = (-C209.x) - (-clip.z);
  precise float ramp = ramp_in * ramp_scale;
  float fade = isnan(ramp) ? 0.0 : clamp(ramp, 0.0, 1.0);
  precise float scale = C255.y - fade;
  out_uv = vec4(halves.y, halves.x, scale, 0.0);
  float cmp = (-abs(ramp_in) > C255.z) ? 1.0 : 0.0;
  out_flag_a = vec4(cmp, 0.0, 0.0, 0.0);
  out_flag_b = vec4(cmp, cmp, cmp, 0.0);

  precise float up_scaled = C255.x * up;
  precise float facing = up_scaled + C255.x;
  vec3 lit = AvLit(AvLightTerms(nw), AvAmbient(facing));
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise vec4 shaded = vec4(lit, alpha_scale) * color;
  out_color = shaded;
}
