#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: every shader of the frame ours): the game's flat-colour primitive (yDebugPrimitive,
// guest pixel shader A4A965C189287B99; the clears of the passes and the plane shadow's mask), through the shader swap
// (src\scene_swap.h): the colour the vertex shader passes. No float constants.

layout(location = 0) in vec4 in_color;

void main() { XeOutputColor(in_color); }
