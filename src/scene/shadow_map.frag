#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the shadow maps' depth (the game's yShadowMap,
// guest pixel shader DA494C9383E18961; the wrestlers' and ring's 512x512 maps), through the shader swap
// (src\scene_swap.h): the depth the vertex shader passes (first interpolator's x) mapped from c168.x..c168.y to 0..1,
// in red and green; blue and alpha 1. The console's 0 x infinity = 0 is kept for an empty range. Float constants
// (set 1 binding 2): 168.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C168 fc.c[0]  // x: near depth, y: far depth

layout(location = 0) in vec4 in_depth;  // x: the light-space depth

void main() {
  float inv_range = 1.0 / (C168.y - C168.x);
  float d = in_depth.x - C168.x;
  float v = (d == 0.0 || inv_range == 0.0) ? 0.0 : d * inv_range;
  XeOutputColor(vec4(v, v, 1.0, 1.0));
}
