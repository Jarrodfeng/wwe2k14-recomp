#version 450
// Round blob shadow under a wrestler, vertex stage. See docs\renderer\effects\yCircleShadow.md.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ShadowParam;         // x strength, y ground height, z fade height, w fade power
  vec4 ViewProj[4];
  vec4 WorldPosArray[32];   // the tracked body points (feet, hips ...)
} vc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in float inBlend;     // 0..1: mix between the two body points
layout(location = 2) in vec2 inPointIds;   // which two body points the vertex follows
layout(location = 3) in vec3 inUvStrength; // xy: disc coordinate, z: this vertex's strength

layout(location = 0) out vec4 outDisc;
layout(location = 1) out vec4 outAlpha;

void main() {
  int first = int(inPointIds.x);
  int second = int(inPointIds.y);
  vec3 world = inPosition + inBlend * vc.WorldPosArray[first].xyz + (1.0 - inBlend) * vc.WorldPosArray[second].xyz;
  vec4 p = vec4(world, 1.0);
  vec4 clip = vec4(dot(vc.ViewProj[0], p), dot(vc.ViewProj[1], p), dot(vc.ViewProj[2], p), dot(vc.ViewProj[3], p));

  // The shadow fades with height above the ground, and vanishes below a cut-off alpha.
  float height = clamp(abs(world.y - vc.ShadowParam.y) / vc.ShadowParam.z, 0.0, 1.0);
  float alpha = inUvStrength.z * vc.ShadowParam.x * fx_pow(1.0 - height, vc.ShadowParam.w);

  gl_Position = alpha >= 0.01 ? clip : vec4(0.0);
  outDisc = vec4(inUvStrength.xy, 0.0, 0.0);
  outAlpha = vec4(alpha, 0.0, 0.0, 0.0);
}
