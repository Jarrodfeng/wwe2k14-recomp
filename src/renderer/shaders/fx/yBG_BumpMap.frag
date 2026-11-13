#version 450
// Arena surface BumpMap, pixel stage. See fx_bg.glsl and docs/renderer/effects/yBG.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define BG_BUMP
#define FX_NO_LM
#define BG_FRAGMENT
#include "fx_bg.glsl"
void main() { bg_fragment_main(); }
