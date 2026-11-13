#version 450
// Arena surface ReflectLM, pixel stage. See fx_bg.glsl and docs/renderer/effects/yBG.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define FX_SPECULAR
#define FX_REFLECT
#define FX_FIXED_LOD
#define BG_FRAGMENT
#include "fx_bg.glsl"
void main() { bg_fragment_main(); }
