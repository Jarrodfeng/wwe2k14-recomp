#version 450
// Particle sprites (sparks, smoke, confetti), vertex stage. See docs\renderer\effects\yParticle.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 LocalWorld[4];
  vec4 ZOffset;           // x depth pulled towards the camera
  vec4 UseNormal;         // x below 0.9: flat brightness; otherwise lit by the facing normal
  vec4 LightVector;
} vc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec2 inUv2;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec2 outUv2;
layout(location = 2) out vec4 outColor;

void main() {
  vec4 local = vec4(inPosition, 1.0);
  vec4 world = vec4(dot(vc.LocalWorld[0], local), dot(vc.LocalWorld[1], local), dot(vc.LocalWorld[2], local), dot(vc.LocalWorld[3], local));
  gl_Position = vec4(dot(vc.ViewProj[0], world), dot(vc.ViewProj[1], world), dot(vc.ViewProj[2], world) - vc.ZOffset.x, dot(vc.ViewProj[3], world));

  float brightness = 1.0;
  if (vc.UseNormal.x >= 0.9) {
    // The sprite's facing normal is the third world axis, flipped to face the eye; the eye direction is taken from the object position and the view matrix's translation row.
    vec3 toEye = fx_normalize(inPosition - vc.ViewProj[3].xyz);
    vec3 normal = vc.LocalWorld[2].xyz;
    if (dot(toEye, normal) < 0.0) normal = -normal;
    vec3 lightDir = vc.LightVector.xyz * inversesqrt(dot(vc.LightVector, vc.LightVector));
    brightness = clamp(2.0 * max(dot(lightDir, normal), 0.1), 0.0, 1.0);
  }
  outUv = inUv;
  outUv2 = inUv2;
  outColor = vec4(inColor.rgb * brightness, inColor.a);
}
