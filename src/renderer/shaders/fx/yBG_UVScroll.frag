#version 450
// Arena surface UVScroll, pixel stage. See fx_bg.glsl and docs/renderer/effects/yBG.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define BG_FRAGMENT
#define FX_SCROLL   // every shader of this effect scrolls
#include "fx_bg.glsl"
void main() { bg_fragment_main(); }
