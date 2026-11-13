#version 450
// Wrestler gear (armour, pads, props on the body), vertex stage. All of it is in fx_gear.glsl; see docs\renderer\effects\yCh_gear_flex.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define GEAR_VERTEX
#include "fx_gear.glsl"
