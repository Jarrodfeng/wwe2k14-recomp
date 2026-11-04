#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yCh_gear (9-dword vertices) (guest vertex shader A6767C4D4522DFAA; 3 / 3 / 4 draws a frame in
// Brock's entrance / Bryan's entrance / the match). Pixel side: unknown. Hand-written from our reading of the guest shader (the body is
// vs_ch_gear.glsl; the skinning is vs_ch.glsl); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
// The gear shader without blend weights (zero weights); bytes at dword 4, normal 5, tangent 6, texture coordinate 8.
#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#define CH_SKIN_STYLE 1
#define CH_STRIDE 9
#define CH_WEIGHTS 0
#define CH_BYTES 4
#define CH_NORMAL 5
#define CH_TANGENT 6
#define CH_UV 8
#include "vs_ch.glsl"
#include "vs_ch_gear.glsl"
