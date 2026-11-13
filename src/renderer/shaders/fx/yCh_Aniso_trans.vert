#version 450
// yCh_Aniso_trans vertex stage: the shared transparent-capable character vertex work (fx_trans_vs.glsl) with the hair tangent. See docs/renderer/effects/yCh_Anisotropic.md.
#extension GL_GOOGLE_include_directive : require
#define FX_HAIR
#include "fx_trans_vs.glsl"
