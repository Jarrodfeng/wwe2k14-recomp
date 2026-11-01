#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the crowd's textured copy (the game's yAudience
// copy, guest pixel shader 7ABDFEFBCADB4BF9; the 512x512 atlas the crowd and the audience planes read), through the
// shader swap (src\scene_swap.h): the texture times the vertex colour. No float constants.

layout(set = 3, binding = 1000) uniform texture2DArray t_source;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_source;

layout(location = 0) in vec4 in_uv;     // xy: the source's coordinate
layout(location = 1) in vec4 in_color;  // the vertex colour

void main() {
  XeOutputColor(texture(sampler2DArray(t_source, s_source), vec3(in_uv.xy, 0.0)) * in_color);
}
