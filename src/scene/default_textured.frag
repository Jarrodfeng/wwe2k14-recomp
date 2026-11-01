#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: every shader of the frame ours): the game's plain textured material (yDefault, guest
// pixel shader 9A39D20F274CF7D9; the overlays drawn after the post chain), through the shader swap (src\scene_swap.h):
// the texture times the vertex colour (the seventh interpolator). No float constants.

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_color;

layout(location = 0) in vec4 in_uv;     // xy: the texture's coordinate
layout(location = 6) in vec4 in_color;  // the vertex colour

void main() { XeOutputColor(texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0)) * in_color); }
