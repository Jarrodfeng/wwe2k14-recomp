#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: every shader of the frame ours): a video sprite's colour conversion with a fourth
// plane for alpha (the game's ySprite, guest pixel shader B57EADC321958E8F), through the shader swap
// (src\scene_swap.h): three planes (fetch constants 0, 1, 2: luma and the two chroma planes, by how they are weighted)
// read at one coordinate and turned into red / green / blue by the matrix in c253 / c254 / c255 - red and blue a
// weighted sum each, green the luma term less the two chroma terms, the same conversion as src\scene\movie_yuv.frag -
// times the vertex colour (the second interpolator); alpha = a fourth plane (fetch constant 3, its red) times the vertex
// alpha. What the plane pictures are drawn over is unconfirmed. Float constants (set 1 binding 2): 253, 254, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[3]; } fc;
#define C253 fc.c[0]
#define C254 fc.c[1]
#define C255 fc.c[2]

layout(set = 3, binding = 1000) uniform texture2DArray t_plane0;  // fetch constant 0
layout(set = 3, binding = 1001) uniform texture2DArray t_plane1;  // 1
layout(set = 3, binding = 1002) uniform texture2DArray t_plane2;  // 2
layout(set = 3, binding = 1003) uniform texture2DArray t_alpha;   // 3: the alpha plane
layout(set = 3, binding = 1200) uniform sampler s_plane0;
layout(set = 3, binding = 1201) uniform sampler s_plane1;
layout(set = 3, binding = 1202) uniform sampler s_plane2;
layout(set = 3, binding = 1203) uniform sampler s_alpha;

layout(location = 0) in vec4 in_uv;     // xy: the picture's coordinate
layout(location = 1) in vec4 in_color;  // the vertex colour

void main() {
  vec3 coord = vec3(in_uv.xy, 0.0);
  float alpha_plane = texture(sampler2DArray(t_alpha, s_alpha), coord).x;
  float p2 = texture(sampler2DArray(t_plane2, s_plane2), coord).x;
  float p1 = texture(sampler2DArray(t_plane1, s_plane1), coord).x;
  float p0 = texture(sampler2DArray(t_plane0, s_plane0), coord).x;
  // The three terms of green, each a plane times its weight plus its offset.
  vec3 g_terms = vec3(p0 * C254.z + C255.x, p1 * C254.x + C255.y, p2 * C254.y + C255.z);
  float red = p2 * C253.z + (p0 * C254.z + C253.x);
  float blue = p1 * C253.w + (p0 * C254.z + C253.y);
  float green = (g_terms.x - g_terms.y) - g_terms.z;
  XeOutputColor(vec4(vec3(red, green, blue) * in_color.rgb, alpha_plane * in_color.a));
}
