#version 450
// Arena surface UVScrollLM, vertex stage. See fx_bg.glsl and docs/renderer/effects/yBG.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define BG_VERTEX
#define FX_SCROLL   // every shader of this effect scrolls
#include "fx_bg.glsl"
void main() { bg_vertex_main(); }
