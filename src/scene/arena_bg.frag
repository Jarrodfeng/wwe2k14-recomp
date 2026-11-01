#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the arena's plain background material (the game's yBG_Default,
// guest pixel shader F335375CE1CC39E4; in matches the audience planes and the arena screen, which samples the previous
// frame's scene), through the shader swap (src\scene_swap.h): the colour texture at the shader's own LOD bias (c248.x)
// times the vertex colour. Written from the shader's microcode (read locally), checked against the SDK translator's
// SPIR-V of it: as the wrestler materials, unsigned textures, no exponent adjust, no fetch-constant LOD bias and no
// rounding nudge on the coordinate. The colour goes out through the draw's own alpha test / exponent bias / gamma.
// Float constants (set 1 binding 2): 248.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C248 fc.c[0]  // x: LOD bias of the colour texture

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_color;

layout(location = 0) in vec4 in_uv;     // xy: the colour's coordinate
layout(location = 3) in vec4 in_color;  // the vertex colour

void main() {
  XeOutputColor(texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x) * in_color);
}
