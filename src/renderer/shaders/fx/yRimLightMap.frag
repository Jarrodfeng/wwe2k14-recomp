#version 450
// Rim-light depth pass, pixel stage: red 0, green the depth fade. See docs\renderer\effects\yRimLightMap.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec4 inDepth;
layout(location = 0) out vec4 outColor;

void main() {
  outColor = vec4(0.0, inDepth.x, 0.0, 1.0);
}
