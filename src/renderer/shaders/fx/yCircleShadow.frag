#version 450
// Round blob shadow, pixel stage: a soft Gaussian disc written as black with alpha. See docs\renderer\effects\yCircleShadow.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(location = 0) in vec4 inDisc;
layout(location = 1) in vec4 inAlpha;
layout(location = 0) out vec4 outColor;

void main() {
  outColor = vec4(0.0, 0.0, 0.0, exp2(-dot(inDisc.xy, inDisc.xy) * 0.721348) * inAlpha.x);
}
