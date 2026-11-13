#version 450
// Shadow-receiving prop, vertex stage. See the yProp_PSM effect page.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define FX_PSM_VERT
#include "fx_prop.glsl"
