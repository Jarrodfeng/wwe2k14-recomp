#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: every shader of the frame ours): the entrance's 2D crowd sprites (the game's
// yAudience, guest pixel shader EFC4EFCB5FC51945; vertex shader D5E2FCC9F76BC40C, each sprite one of the crowd's 64x64
// renders), through the shader swap (src\scene_swap.h): the sprite times the vertex colour times the arena's lighting
// map (fetch constant 13, at the first interpolator's zw) scaled by its strength c184.z; alpha = the sprite's times the
// vertex alpha. Float constants (set 1 binding 2): 184.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C184 fc.c[0]  // z: strength of the lighting map

layout(set = 3, binding = 1000) uniform texture2DArray t_sprite;  // fetch constant 0
layout(set = 3, binding = 1013) uniform texture2DArray t_light;   // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_sprite;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_uv;     // xy: the sprite's coordinate, zw: the lighting map's
layout(location = 1) in vec4 in_color;  // the vertex colour

void main() {
  vec3 light = texture(sampler2DArray(t_light, s_light), vec3(in_uv.zw, 0.0)).rgb * C184.z;
  vec4 sprite = texture(sampler2DArray(t_sprite, s_sprite), vec3(in_uv.xy, 0.0));
  XeOutputColor(vec4(sprite.rgb * in_color.rgb * light, sprite.a * in_color.a));
}
