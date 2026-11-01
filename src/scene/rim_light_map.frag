#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the wrestlers' rim light map (the game's
// yRimLightMap, guest pixel shader D03E7D4C719B0EDB; the 512x256 pass the wrestler materials read blurred), through
// the shader swap (src\scene_swap.h): green = the first interpolator's x, the rest 0 and alpha 1. No float constants.

layout(location = 0) in vec4 in_value;  // x: the rim term the vertex shader made

void main() { XeOutputColor(vec4(0.0, in_value.x, 0.0, 1.0)); }
