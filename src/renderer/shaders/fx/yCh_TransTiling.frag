#version 450
// yCh_TransTiling pixel stage: the shared transparent-capable character pixel work (fx_trans_ps.glsl) with the tiled noise; a logo is blended in by its alpha.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#include "fx_character.glsl"
#include "fx_trans.glsl"
#define FX_NOISE
#define FX_LOGO_BLEND
#include "fx_trans_ps.glsl"
