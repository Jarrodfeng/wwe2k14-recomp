#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the props' lightmapped material with a projected shadow and a
// detail layer (the game's yProp_PSMLM, guest pixel shader 4E1A94BC95B4C2E9; "PSM" unconfirmed, the third map is
// read at a projected coordinate, so a projected shadow map), through the shader swap (src\scene_swap.h), in order:
//   - the projected coordinate: the third interpolator's xy times the reciprocal of its z (the console's
//     0 x infinity = 0 kept, Mul0); the shadow map (fetch constant 12) read there, its red = how lit the surface is,
//     taken as c255.x minus it;
//   - the detail map (fetch constant 6, second coordinate) unpacked as x * c254.x + c254.z, times c207.z;
//   - the colour map (fetch constant 0, first coordinate) at the register LOD c254.w (a texture bias, as the
//     translator applies it) times the vertex colour, plus the detail, floored at c254.y, times the lit amount;
//   - times the lighting map (fetch constant 13, sixth coordinate) scaled by its strength c184.z.
// Alpha = the colour map's alpha times the vertex alpha. The vertex colour is the seventh interpolator, which the
// guest reads with centroid sampling. Float constants (set 1 binding 2): 184, 207, 254, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[4]; } fc;
#define C184 fc.c[0]  // z: strength of the lighting map
#define C207 fc.c[1]  // z: strength of the detail
#define C254 fc.c[2]  // x, z: the detail's unpack scale and offset; y: floor of the colour sum; w: LOD bias of the colour map
#define C255 fc.c[3]  // x: one (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0
layout(set = 3, binding = 1006) uniform texture2DArray t_detail;  // 6: the detail map
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;  // 12: the projected shadow map
layout(set = 3, binding = 1013) uniform texture2DArray t_light;   // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1206) uniform sampler s_detail;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_uv;               // xy: the colour map's coordinate
layout(location = 1) in vec4 in_detail_uv;        // xy: the detail map's coordinate
layout(location = 2) in vec4 in_shadow;           // xyz: the shadow map's projected coordinate
layout(location = 5) in vec4 in_light_uv;         // xy: the lighting map's coordinate
layout(location = 6) centroid in vec4 in_color;   // the vertex colour

void main() {
  float inv_z = 1.0 / in_shadow.z;
  vec2 shadow_uv = vec2(Mul0(inv_z, in_shadow.x), Mul0(inv_z, in_shadow.y));
  vec3 detail = texture(sampler2DArray(t_detail, s_detail), vec3(in_detail_uv.xy, 0.0)).rgb;
  vec3 light = texture(sampler2DArray(t_light, s_light), vec3(in_light_uv.xy, 0.0)).rgb;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C254.w);
  float shadow = texture(sampler2DArray(t_shadow, s_shadow), vec3(shadow_uv, 0.0)).r;
  float lit = C255.x - shadow;
  vec3 detail_add = (detail * C254.x + C254.z) * C207.z;
  vec3 rgb = max(color.rgb * in_color.rgb + detail_add, vec3(C254.y)) * lit;
  XeOutputColor(vec4(rgb * (light * C184.z), color.a * in_color.a));
}
