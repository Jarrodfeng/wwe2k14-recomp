#version 450
// Particle sprites, pixel stage: texture times vertex colour. See docs\renderer\effects\yParticle.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
  outColor = texture(textures[0], inUv) * inColor;
}
