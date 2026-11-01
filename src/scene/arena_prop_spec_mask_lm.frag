#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the props' lightmapped material with a masked highlight (the
// game's yProp_DefaultLM, guest pixel shader F7FA89EF70876EFF; a second yProp_DefaultLM variant besides
// arena_prop_lm.frag), through the shader swap (src\scene_swap.h): arena_prop_spec_mask.frag's maths - the colour
// texture (at the shader's own LOD bias, c248.x) times the vertex colour, plus the highlight colour (c200 times c201.x
// times the colour's alpha, the highlight mask) times the highlight (the normal against the half vector of the view
// and the light c8, floored at c255.x and raised to c202.x) - then times the lighting map (fetch constant 13, fourth
// interpolator's coordinate) scaled by its strength c184.z. Alpha = the vertex alpha alone. The console's
// 0 x infinity = 0 is kept in the power (PowAbs). Float constants (set 1 binding 2): 8, 184, 200, 201, 202, 248, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[7]; } fc;
#define C8   fc.c[0]  // xyz: the light's direction
#define C184 fc.c[1]  // z: strength of the lighting map
#define C200 fc.c[2]  // xyz: highlight colour
#define C201 fc.c[3]  // x: highlight strength
#define C202 fc.c[4]  // x: highlight power
#define C248 fc.c[5]  // x: LOD bias of the colour texture
#define C255 fc.c[6]  // x: floor of the highlight's dot product (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0
layout(set = 3, binding = 1013) uniform texture2DArray t_light;  // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_uv;                // xy: the colour's coordinate
layout(location = 1) in vec4 in_normal;            // xyz: the surface normal
layout(location = 2) in vec4 in_view;              // xyz: towards the camera
layout(location = 3) in vec4 in_light_uv;          // xy: the lighting map's coordinate
layout(location = 4) centroid in vec4 in_color;    // the vertex colour (centroid sampling, as the guest reads it)

void main() {
  vec3 light = texture(sampler2DArray(t_light, s_light), vec3(in_light_uv.xy, 0.0)).rgb * C184.z;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  float highlight = ArenaHighlight(in_normal.xyz, in_view.xyz, C8.xyz, C255.x, C202.x);
  vec3 rgb = ((color.a * C201.x) * C200.xyz) * highlight + color.rgb * in_color.rgb;
  XeOutputColor(vec4(light * rgb, in_color.a));
}
