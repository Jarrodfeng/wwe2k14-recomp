#version 450
// Wrestler skin with the muscle option, vertex stage. The shader is in fx_skin.glsl (shared with yCh_skin). See docs\renderer\effects\yCh_skin.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#define SKIN_VERTEX
#include "fx_skin.glsl"
