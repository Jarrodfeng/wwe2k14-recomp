#version 450
#extension GL_GOOGLE_include_directive : require
// Session 95 (post lite, wwe2k14_post_lite): the ORIGINAL match combine (1233F68C15FDF948) with the exact early-outs (E1: no blur fetch where its weight is 0, no tint where k is 0).
#define ORIGINAL 1
#define LITE 1
#include "combine.glsl"
