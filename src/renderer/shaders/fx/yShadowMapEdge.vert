#version 450
// Shadow-map edge detection, vertex stage: a full-screen quad that hands the pixel stage four diagonal texture coordinates. See docs\renderer\effects\yShadowMapEdge.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 OffsetTex;           // xy: one texel step in u and v
} vc;

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec4 outLowDiagonal;     // (u-dx, v-dy) and (u+dx, v+dy)
layout(location = 1) out vec4 outHighDiagonal;    // (u-dx, v+dy) and (u+dx, v-dy)

void main() {
  gl_Position = inPosition;
  vec2 lo = inUv - vc.OffsetTex.xy;
  vec2 hi = inUv + vc.OffsetTex.xy;
  outLowDiagonal = vec4(lo, hi);
  outHighDiagonal = vec4(lo.x, hi.y, hi.x, lo.y);
}
