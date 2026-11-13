#version 450
// Shadow-map edge detection, pixel stage: a Roberts-cross edge strength from the four diagonal neighbours. See docs\renderer\effects\yShadowMapEdge.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) in vec4 inLowDiagonal;
layout(location = 1) in vec4 inHighDiagonal;
layout(location = 0) out vec4 outColor;

void main() {
  float lowLow = texture(textures[0], inLowDiagonal.xy).x;      // (u-dx, v-dy)
  float highHigh = texture(textures[0], inLowDiagonal.zw).x;    // (u+dx, v+dy)
  float lowHigh = texture(textures[0], inHighDiagonal.xy).x;    // (u-dx, v+dy)
  float highLow = texture(textures[0], inHighDiagonal.zw).x;    // (u+dx, v-dy)
  float a = lowLow - highHigh;
  float b = lowHigh - highLow;
  outColor = vec4(a * a + b * b);
}
