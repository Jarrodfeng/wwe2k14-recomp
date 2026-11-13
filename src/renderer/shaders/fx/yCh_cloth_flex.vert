#version 450
// Wrestler cloth (clothing that moves on top of the body), vertex stage. All of it is in fx_gear.glsl; see docs\renderer\effects\yCh_cloth_flex.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define FX_CLOTH
#define GEAR_VERTEX
#include "fx_gear.glsl"
