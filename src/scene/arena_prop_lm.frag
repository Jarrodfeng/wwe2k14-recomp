#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the props' lightmapped material (the game's yProp_DefaultLM, guest
// pixel shader 047BBF321B125654), through the shader swap (src\scene_swap.h): arena_bg_lm.frag's maths with the
// lighting map's coordinate in the second interpolator and the colour in the third. Float constants (set 1 binding 2):
// 184, 248.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[2]; } fc;
#define C184 fc.c[0]  // z: strength of the lighting map
#define C248 fc.c[1]  // x: LOD bias of the colour texture

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0
layout(set = 3, binding = 1013) uniform texture2DArray t_light;  // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_uv;        // xy: the colour's coordinate
layout(location = 1) in vec4 in_light_uv;  // xy: the lighting map's coordinate
layout(location = 2) in vec4 in_color;     // the vertex colour

void main() {
  vec3 light = texture(sampler2DArray(t_light, s_light), vec3(in_light_uv.xy, 0.0)).rgb * C184.z;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  XeOutputColor(vec4(color.rgb * in_color.rgb * light, color.a * in_color.a));
}
