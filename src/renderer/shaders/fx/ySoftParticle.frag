#version 450
// Soft particle sprites, pixel stage: texture times colour, faded by how far the sprite is in front of the scene depth. See docs\renderer\effects\ySoftParticle.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 FadeOutDistance;     // x: how far in front of the scene the sprite is fully opaque
  vec4 Proj[3];             // rows 0-2 of the projection matrix, of which only row 2 (z, w) is used to turn a depth value back into a distance
} fc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) in vec4 inUv;
layout(location = 1) in vec4 inScreen;
layout(location = 2) in vec4 inViewPos;
layout(location = 3) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 particle = texture(textures[0], inUv.xy);
  vec2 screenUv = 0.5 * inScreen.xy / inScreen.w + 0.5;
  float sceneDepth = texture(textures[1], screenUv).x;
  float sceneDistance = fc.Proj[2].w / ((1.0 - fc.Proj[2].z) - sceneDepth);
  float gap = min(abs(sceneDistance - inViewPos.z), fc.FadeOutDistance.x);
  outColor = vec4(particle.rgb * inColor.rgb, gap * (inColor.a / fc.FadeOutDistance.x) * particle.a);
}
