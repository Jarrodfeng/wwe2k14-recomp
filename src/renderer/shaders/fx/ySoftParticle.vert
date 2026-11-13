#version 450
// Soft particle sprites (smoke, dust, haze that fade where they meet geometry), vertex stage. See docs\renderer\effects\ySoftParticle.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 LocalWorld[4];
  vec4 LocalWorldView[4];
  vec4 ZOffset;           // x depth pulled towards the camera
  vec4 UseNormal;         // x below 0.9: flat brightness; otherwise lit by the facing normal
  vec4 LightVector;
} vc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec4 outUv;
layout(location = 1) out vec4 outScreen;     // clip x, minus clip y, clip z, clip w: the pixel stage turns it into the depth-texture coordinate
layout(location = 2) out vec4 outViewPos;
layout(location = 3) out vec4 outColor;

void main() {
  vec4 local = vec4(inPosition, 1.0);
  vec4 world = vec4(dot(vc.LocalWorld[0], local), dot(vc.LocalWorld[1], local), dot(vc.LocalWorld[2], local), dot(vc.LocalWorld[3], local));
  vec4 clip = vec4(dot(vc.ViewProj[0], world), dot(vc.ViewProj[1], world), dot(vc.ViewProj[2], world), dot(vc.ViewProj[3], world));

  float brightness = 1.0;
  if (vc.UseNormal.x >= 0.9) {
    // The facing normal is the third world axis flipped towards the eye; the eye direction comes from the object position and the view matrix's translation row.
    vec3 toEye = fx_normalize(inPosition - vc.ViewProj[3].xyz);
    vec3 normal = vc.LocalWorld[2].xyz;
    if (dot(toEye, normal) < 0.0) normal = -normal;
    vec3 lightDir = vc.LightVector.xyz * inversesqrt(dot(vc.LightVector, vc.LightVector));
    brightness = clamp(2.0 * max(dot(lightDir, normal), 0.1), 0.0, 1.0);
  }

  vec4 viewPos = vec4(dot(vc.LocalWorldView[0], local), dot(vc.LocalWorldView[1], local), dot(vc.LocalWorldView[2], local), dot(vc.LocalWorldView[3], local));
  // A sprite lying flat (its second axis pointing straight down) reports a view depth of zero.
  if (-vc.LocalWorld[1].z > 0.999) viewPos.z = 0.0;

  gl_Position = vec4(clip.xy, clip.z - vc.ZOffset.x, clip.w);
  outUv = vec4(inUv, 0.0, 0.0);
  outScreen = vec4(clip.x, -clip.y, clip.z, clip.w);
  outViewPos = viewPos;
  outColor = vec4(inColor.rgb * brightness, inColor.a);
}
