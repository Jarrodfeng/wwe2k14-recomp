#version 450
// yCh_Anisotropic pixel stage: the shared transparent-capable character pixel work (fx_trans_ps.glsl) with the hair highlights. See docs/renderer/effects/yCh_Anisotropic.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#include "fx_character.glsl"
#include "fx_trans.glsl"
#define FX_HAIR
#define FX_LOGO_BLEND
#include "fx_trans_ps.glsl"
