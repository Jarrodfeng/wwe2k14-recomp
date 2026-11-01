#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the background's two-layer scrolling material (the game's
// yBG_UVScroll, guest pixel shader DB603A91AD911046; the entrance stage), through the shader swap (src\scene_swap.h):
// the second texture laid over the first by its alpha (premultiplied, then divided by the combined alpha), times the
// vertex colour; where the combined alpha is 0 the guest writes a step of the vertex colour's x against c255.y in its
// place, kept here. Written and checked as arena_bg.frag. Float constants (set 1 binding 2): 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C255 fc.c[0]  // x: one; y: the threshold of the empty case (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_base;   // fetch constant 0
layout(set = 3, binding = 1001) uniform texture2DArray t_layer;  // 1
layout(set = 3, binding = 1200) uniform sampler s_base;
layout(set = 3, binding = 1201) uniform sampler s_layer;

layout(location = 0) in vec4 in_uv;        // xy: the first texture's coordinate
layout(location = 1) in vec4 in_layer_uv;  // xy: the second's
layout(location = 2) in vec4 in_color;     // the vertex colour

void main() {
  vec4 base = texture(sampler2DArray(t_base, s_base), vec3(in_uv.xy, 0.0));
  vec4 layer = texture(sampler2DArray(t_layer, s_layer), vec3(in_layer_uv.xy, 0.0));
  float under = C255.x - layer.a;
  float alpha = under * base.a + layer.a;
  vec4 result;
  if (alpha == 0.0) {
    result = vec4(-abs(in_color.x) > C255.y ? 1.0 : 0.0);
  } else {
    vec3 rgb = (base.rgb * base.a) * under + layer.rgb * layer.a;
    result = vec4(rgb * (1.0 / alpha), alpha);
  }
  XeOutputColor(result * in_color);
}
