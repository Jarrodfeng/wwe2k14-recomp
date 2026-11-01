#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the props' material that receives the wrestlers' blurred plane
// shadow (the game's yProp_PSM, guest pixel shader B87AA657F15D8EC5; the ring mat), through the shader swap
// (src\scene_swap.h). In order: the shadow map (fetch constant 12) is read at the projected coordinate (xy over the
// third interpolator's z; the console's 0 x infinity = 0 kept in the division); a second map (fetch constant 6, at its
// own coordinate) is re-ranged (times c254.x plus c254.z) and scaled by c207.z; the colour texture (fetch constant 0)
// at the LOD bias c254.w times the vertex colour is added to that, floored at c254.y; the result is darkened by
// (c255.x minus the shadow) and alpha = the colour's times the vertex alpha. What the second map is (a lighting or
// ambient map, from the way it is re-ranged) is unconfirmed. Float constants (set 1 binding 2): 207, 254, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[3]; } fc;
#define C207 fc.c[0]  // z: scale of the second map's term
#define C254 fc.c[1]  // x: second map's range scale, y: floor of the sum, z: its range offset, w: LOD bias of the colour
#define C255 fc.c[2]  // x: the shadow's complement base (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0
layout(set = 3, binding = 1006) uniform texture2DArray t_second;  // 6: second map (unconfirmed: lighting / ambient)
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;  // 12: the plane shadow
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1206) uniform sampler s_second;
layout(set = 3, binding = 1212) uniform sampler s_shadow;

layout(location = 0) in vec4 in_uv;         // xy: the colour's coordinate
layout(location = 1) in vec4 in_second_uv;  // xy: the second map's coordinate
layout(location = 2) in vec4 in_shadow;     // xyz: the shadow's projected coordinate (x, y, divisor z)
layout(location = 5) in vec4 in_color;      // the vertex colour

void main() {
  float inv_w = 1.0 / in_shadow.z;
  vec2 shadow_uv = vec2(Mul0(inv_w, in_shadow.x), Mul0(inv_w, in_shadow.y));
  vec3 second = texture(sampler2DArray(t_second, s_second), vec3(in_second_uv.xy, 0.0)).rgb;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C254.w);
  float shadow = texture(sampler2DArray(t_shadow, s_shadow), vec3(shadow_uv, 0.0)).x;
  vec3 lit = (second * C254.x + C254.z) * C207.z + color.rgb * in_color.rgb;
  lit = mix(vec3(C254.y), lit, greaterThanEqual(lit, vec3(C254.y)));  // the console's max
  XeOutputColor(vec4(lit * (C255.x - shadow), color.a * in_color.a));
}
