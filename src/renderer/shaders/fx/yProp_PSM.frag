#version 450
// Shadow-receiving prop, pixel stage. See the yProp_PSM effect page.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define FX_PSM_FRAG
#include "fx_prop.glsl"
