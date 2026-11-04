#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yProp_BumpMap (no depth fade) (guest vertex shader 962408F804CE9071; 9 / 12 / 6 draws a frame in
// Brock's entrance / Bryan's entrance / the match). Pixel side: 741439F5 (bench census). Hand-written from our reading of the guest shader (the body is
// vs_prop_bump.glsl); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
// The form without the depth fade (interpolator 0 carries 1 in z).
#define PROP_BUMP_NO_FADE
#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#include "vs_prop.glsl"
#include "vs_prop_bump.glsl"
