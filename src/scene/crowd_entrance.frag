#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (SCENE_SHADERS_PLAN.md step 1; Jarrod: "Our own scene shaders"): the entrance crowd's material, in place
// of guest pixel shader 803205E3BEA932C8 - as the match's (crowd_match.frag), times the arena's lighting map read at
// the member's second coordinate and scaled by the lighting map's strength (c184.z, as the vertex shader scales it).
// Interface: crowd_match.frag's, plus set 1 binding 2 = the guest shader's float constants packed in register order.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[1]; } fc;  // c184
#define C184 fc.c[0]

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0: the member's colour
layout(set = 3, binding = 1013) uniform texture2DArray t_light;  // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_texcoord;  // xy: the colour's coordinate, zw: the lighting map's
layout(location = 1) in vec4 in_light;

void main() {
  vec3 light_map = texture(sampler2DArray(t_light, s_light), vec3(in_texcoord.zw, 0.0)).rgb * C184.z;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_texcoord.xy, 0.0));
  XeOutputColor(vec4(color.rgb * in_light.rgb * light_map, color.a));  // (Session 87, stage 4: its own epilogue, as crowd_match.frag)
}
