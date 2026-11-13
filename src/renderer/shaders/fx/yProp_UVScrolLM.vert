#version 450
// Prop surface with scrolling uv, vertex stage. See docs\renderer\effects\yProp.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define FX_SCROLL
#define FX_PROP_VERT
#include "fx_prop.glsl"
