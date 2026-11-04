#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the crowd's spectator figures in the match (yAudDefault) (guest vertex shader 0E2A3DF5BAA2CA18;
// 20 / 20 / 30 draws a frame in Brock's entrance / Bryan's entrance / the match). Pixel side: the match crowd pixel shader ED94EC2F6D52E14F (the census pair). Hand-written from our reading of the guest
// shader (the body is vs_crowd_figure.glsl); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
//
// Interpolators: 0 = (u, v); 1 = the lighting-map colour x c184.z x the vertex colour x the light (alpha: the map alpha x c184.z x the vertex alpha).
#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#include "xenos_vs_tex.glsl"
#include "vs_ch.glsl"

#include "vs_crowd_figure.glsl"
