#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the props' material with a highlight (the game's yProp_Default,
// guest pixel shader B73643DDFFC89BAC; the ring posts among others), through the shader swap (src\scene_swap.h): the
// colour texture at the shader's own LOD bias (c248.x) times the vertex colour, plus the highlight colour (c200 times
// c201.x) times the highlight (the normal against the half vector of the view and the light c8, floored at c255.x and
// raised to c202.x); alpha = the colour's times the vertex alpha. Written and checked as arena_bg.frag; the console's
// 0 x infinity = 0 is kept in the power (PowAbs). Float constants (set 1 binding 2): 8, 200, 201, 202, 248, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[6]; } fc;
#define C8   fc.c[0]  // xyz: the light's direction
#define C200 fc.c[1]  // xyz: highlight colour
#define C201 fc.c[2]  // x: highlight strength
#define C202 fc.c[3]  // x: highlight power
#define C248 fc.c[4]  // x: LOD bias of the colour texture
#define C255 fc.c[5]  // x: floor of the highlight's dot product (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_color;

layout(location = 0) in vec4 in_uv;      // xy: the colour's coordinate
layout(location = 1) in vec4 in_normal;  // xyz: the surface normal
layout(location = 2) in vec4 in_view;    // xyz: towards the camera
layout(location = 3) in vec4 in_color;   // the vertex colour

void main() {
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  float highlight = ArenaHighlight(in_normal.xyz, in_view.xyz, C8.xyz, C255.x, C202.x);
  vec3 rgb = (C200.xyz * C201.x) * highlight + color.rgb * in_color.rgb;
  XeOutputColor(vec4(rgb, color.a * in_color.a));
}
