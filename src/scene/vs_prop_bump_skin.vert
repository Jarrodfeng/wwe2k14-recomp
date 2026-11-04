#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yProp_BumpMap (guest vertex shader A0C9935FB47C5A28; 24 / 24 / 26 draws a frame in
// Brock's entrance / Bryan's entrance / the match). Pixel side: the prop bump pixel shader AFB4E9E0 (bench census). Hand-written from our reading of the guest shader (the body is
// vs_prop_bump.glsl); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
// The form with the depth fade.

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#include "vs_prop.glsl"
#include "vs_prop_bump.glsl"
