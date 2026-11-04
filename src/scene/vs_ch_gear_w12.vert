#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yCh_gear (12-dword vertices) (guest vertex shader F709D14F30FCC95C; 19 / 28 / 27 draws a frame in
// Brock's entrance / Bryan's entrance / the match). Pixel side: B1F58C0A and 1B66FF87 (bench census). Hand-written from our reading of the guest shader (the body is
// vs_ch_gear.glsl; the skinning is vs_ch.glsl); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
// The gear shader with blend weights in the vertex (dword 4); bytes at 7, normal 8, tangent 9, texture coordinate 11.
#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#define CH_SKIN_STYLE 1
#define CH_STRIDE 12
#define CH_WEIGHTS 1
#define CH_BYTES 7
#define CH_NORMAL 8
#define CH_TANGENT 9
#define CH_UV 11
#include "vs_ch.glsl"
#include "vs_ch_gear.glsl"
