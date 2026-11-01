#version 450
#extension GL_GOOGLE_include_directive : require
// Session 80 (SCENE_SHADERS_PLAN.md, diagnostic only - wwe2k14_scene_shaders_probe 1): a flat grey in place of a scene
// material's pixel shader, through the draw's own output step (xenos_ps.glsl). Pictures are wrong on purpose: the GPU
// time it saves on the Surface is the most our own shader for that material could save on its pixel side.
#include "xenos_ps.glsl"

void main() {
  XeOutputColor(vec4(0.5, 0.5, 0.5, 1.0));
}
