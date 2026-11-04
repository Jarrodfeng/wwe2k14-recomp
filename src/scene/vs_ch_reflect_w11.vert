#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yCh_Reflect (11-dword vertices) (guest vertex shader 9CB11A97F230260C; 0 / 2 / 4 draws a frame in
// Brock's entrance / Bryan's entrance / the match). Pixel side: unknown. Hand-written from our reading of the guest shader (the body is
// vs_ch_reflect.glsl; the skinning is vs_ch.glsl); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
// The reflective wrestler with blend weights in the vertex (dword 4); the bytes at 7, the normal at 8, the texture coordinate at 10.
#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#define CH_STRIDE 11
#define CH_WEIGHTS 1
#define CH_BYTES 7
#define CH_NORMAL 8
#define CH_UV 10
#include "vs_ch.glsl"
#include "vs_ch_reflect.glsl"
