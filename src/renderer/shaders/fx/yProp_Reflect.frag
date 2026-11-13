#version 450
// Prop surface, pixel stage. See docs\renderer\effects\yProp.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define FP_LOD
#define FX_PROP_FRAG
#include "fx_prop.glsl"
