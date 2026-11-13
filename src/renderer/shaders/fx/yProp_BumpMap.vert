#version 450
// Normal-mapped prop surface, vertex stage. See docs\renderer\effects\yProp.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define FX_BUMP_VERT
#include "fx_prop.glsl"
