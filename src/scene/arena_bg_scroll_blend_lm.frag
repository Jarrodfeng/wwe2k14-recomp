#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the background's scrolling lightmapped material with two layers
// (the game's yBG_UVScrollLM, guest pixel shader DDBEB05B1F79ABF4; the scrolls are in the vertex shader), through the
// shader swap (src\scene_swap.h): layer B (fetch constant 1) laid over layer A (fetch constant 0) by B's alpha - the
// colour is the two premultiplied colours added and divided by the combined alpha (B.a + (c255.x - B.a) * A.a); where
// that alpha is exactly 0 the colour and alpha are 1 or 0 by whether -|vertex red| is above c255.y. Then times the
// vertex colour and the lighting map (fetch constant 13, third coordinate pair) scaled by its strength c184.z; alpha =
// the combined alpha times the vertex alpha. The console's 0 x anything = 0 is not needed: no product here meets an
// infinity (the reciprocal runs only for a non-zero alpha). Float constants (set 1 binding 2): 184, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[2]; } fc;
#define C184 fc.c[0]  // z: strength of the lighting map
#define C255 fc.c[1]  // x: one (the compiler's literal bank); y: threshold for the empty-alpha case

layout(set = 3, binding = 1000) uniform texture2DArray t_under;  // fetch constant 0: layer A
layout(set = 3, binding = 1001) uniform texture2DArray t_over;   // fetch constant 1: layer B
layout(set = 3, binding = 1013) uniform texture2DArray t_light;  // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_under;
layout(set = 3, binding = 1201) uniform sampler s_over;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_uv_under;  // xy: layer A's coordinate
layout(location = 1) in vec4 in_uv_over;   // xy: layer B's coordinate
layout(location = 2) in vec4 in_light_uv;  // xy: the lighting map's coordinate
layout(location = 3) in vec4 in_color;     // the vertex colour

void main() {
  vec4 under = texture(sampler2DArray(t_under, s_under), vec3(in_uv_under.xy, 0.0));
  vec4 over = texture(sampler2DArray(t_over, s_over), vec3(in_uv_over.xy, 0.0));
  float keep = C255.x - over.a;  // how much of layer A shows through
  float alpha = keep * under.a + over.a;
  vec3 rgb;
  if (alpha == 0.0) {
    float flag = (-abs(in_color.r) > C255.y) ? 1.0 : 0.0;
    rgb = vec3(flag);
    alpha = flag;
  } else {
    rgb = (over.rgb * over.a + (under.rgb * under.a) * keep) * (1.0 / alpha);
  }
  vec3 light = texture(sampler2DArray(t_light, s_light), vec3(in_light_uv.xy, 0.0)).rgb * C184.z;
  XeOutputColor(vec4((rgb * in_color.rgb) * light, in_color.a * alpha));
}
