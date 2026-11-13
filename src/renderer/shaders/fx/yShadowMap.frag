#version 450
// Shadow-map depth pass, pixel stage: writes the view distance scaled to 0..1 between the shadow range's start and end. See docs\renderer\effects\yShadowMap.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 ShadowPrm;           // x range start, y range end
} fc;

layout(location = 0) in vec4 inDepth;
layout(location = 0) out vec4 outColor;

void main() {
  float depth01 = (inDepth.x - fc.ShadowPrm.x) / (fc.ShadowPrm.y - fc.ShadowPrm.x);
  outColor = vec4(depth01, depth01, 1.0, 1.0);
}
