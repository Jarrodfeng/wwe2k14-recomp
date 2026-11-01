#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the soft round shadows under the wrestlers (the
// game's yCircleShadow, guest pixel shader D05BFEA12F4350AD; drawn into the floor's shadow map), through the shader
// swap (src\scene_swap.h): a Gaussian of the distance from the centre - exp2(-(x^2 + y^2 + c255.y) * c255.x) - times
// the vertex strength, in alpha only. Float constants (set 1 binding 2): 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[1]; } fc;
#define C255 fc.c[0]  // x: falloff, y: offset (the compiler's literal bank)

layout(location = 0) in vec4 in_pos;       // xy: position from the centre
layout(location = 1) in vec4 in_strength;  // x: strength

void main() {
  float r2 = dot(in_pos.xy, in_pos.xy) + C255.y;
  XeOutputColor(vec4(0.0, 0.0, 0.0, exp2(-r2 * C255.x) * in_strength.x));
}
