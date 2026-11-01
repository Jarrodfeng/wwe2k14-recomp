#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: every shader of the frame ours): the titantron video's colour conversion (the game's
// yMovieDefault, guest pixel shader B5853824AE01D2D8), through the shader swap (src\scene_swap.h): three planes
// (fetch constants 9, 8, 7: luma and the two chroma planes, by how they are weighted) read at one coordinate and
// turned into red / green / blue by the matrix in c253 / c254 / c255 - red and blue a weighted sum each, green the
// luma term less the two chroma terms - times the vertex colour (the seventh interpolator); alpha = the vertex alpha.
// Float constants (set 1 binding 2): 253, 254, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[3]; } fc;
#define C253 fc.c[0]
#define C254 fc.c[1]
#define C255 fc.c[2]

layout(set = 3, binding = 1007) uniform texture2DArray t_plane7;  // fetch constant 7
layout(set = 3, binding = 1008) uniform texture2DArray t_plane8;  // 8
layout(set = 3, binding = 1009) uniform texture2DArray t_plane9;  // 9
layout(set = 3, binding = 1207) uniform sampler s_plane7;
layout(set = 3, binding = 1208) uniform sampler s_plane8;
layout(set = 3, binding = 1209) uniform sampler s_plane9;

layout(location = 0) in vec4 in_uv;     // xy: the video's coordinate
layout(location = 6) in vec4 in_color;  // the vertex colour

void main() {
  vec3 coord = vec3(in_uv.xy, 0.0);
  float p9 = texture(sampler2DArray(t_plane9, s_plane9), coord).x;
  float p8 = texture(sampler2DArray(t_plane8, s_plane8), coord).x;
  float p7 = texture(sampler2DArray(t_plane7, s_plane7), coord).x;
  // The three terms of green, each a plane times its weight plus its offset.
  vec3 g_terms = vec3(p7 * C254.z + C255.x, p8 * C254.x + C255.y, p9 * C254.y + C255.z);
  float red = p9 * C253.z + (p7 * C254.z + C253.x);
  float blue = p8 * C253.w + (p7 * C254.z + C253.y);
  float green = (g_terms.x - g_terms.y) - g_terms.z;
  XeOutputColor(vec4(vec3(red, green, blue) * in_color.rgb, in_color.a));
}
