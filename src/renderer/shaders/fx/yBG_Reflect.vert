#version 450
// Arena surface Reflect, vertex stage. See fx_bg.glsl and docs/renderer/effects/yBG.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define FX_SPECULAR
#define FX_UV_PLAIN
#define FX_NO_LM
#define FX_DEAD_SLOTS
#define BG_VERTEX
#include "fx_bg.glsl"
void main() { bg_vertex_main(); }
