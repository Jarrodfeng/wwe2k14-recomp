#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the crowd's spectator figures in the entrances (yAudDefault) (guest vertex shader CB5B16DA1312E5AB;
// 0 / 5 / 0 draws a frame in Brock's entrance / Bryan's entrance / the match). Pixel side: unconfirmed (the entrance crowd pixel shader 803205E3BEA932C8 is the likely one). Hand-written from our reading of the guest
// shader (the body is vs_crowd_figure.glsl); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
//
// Interpolators: 0 = (u, v, the lighting-map coordinate); 1 = the light x the vertex colour (alpha: the vertex alpha).
#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#include "vs_ch.glsl"
#define CROWD_FIGURE_ENTRANCE
#include "vs_crowd_figure.glsl"
