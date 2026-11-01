#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the props' plain material (the game's yProp_Default, guest pixel
// shader 468C06E826088769), through the shader swap (src\scene_swap.h): the colour texture at the shader's own LOD bias
// (c248.x) times the vertex colour - arena_bg.frag's maths with the colour in the second interpolator. Written and
// checked as arena_bg.frag. Float constants (set 1 binding 2): 248.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C248 fc.c[0]  // x: LOD bias of the colour texture

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_color;

layout(location = 0) in vec4 in_uv;     // xy: the colour's coordinate
layout(location = 1) in vec4 in_color;  // the vertex colour

void main() {
  XeOutputColor(texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x) * in_color);
}
