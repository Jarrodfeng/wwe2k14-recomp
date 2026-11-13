#version 450
// yCh_Reflect pixel stage: the shared transparent-capable character pixel work (fx_trans_ps.glsl).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#include "fx_character.glsl"
#include "fx_trans.glsl"
#define FX_REFLECT
#include "fx_trans_ps.glsl"
