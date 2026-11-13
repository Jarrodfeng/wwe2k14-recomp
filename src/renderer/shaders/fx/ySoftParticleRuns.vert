#version 450
// Soft particle runs (Session 101, lane 132; docs\renderer\PARTICLES_PROMOTION_MAP.md section 11): the ySoftParticle vertex stage for a RUN of soft particles drawn as one draw.
// Our own code, written from the arithmetic ySoftParticle.vert already holds. The only difference: the particle's LocalWorld rows (the game's c228-c231) and LocalWorldView rows
// (c203-c206), which change from one particle to the next, arrive per vertex in the merged vertex buffer (src\renderer_adapter\particle_runs.h: the game's 24-byte vertex, then
// 4 x vec4, then 4 x vec4), instead of from the constant block. ViewProj, ZOffset, UseNormal and LightVector are the run's. Outputs are ySoftParticle's, so the game's own pixel
// stage (ySoftParticle / base, which reads the scene depth in slot 1) draws it.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 ZOffset;           // x depth pulled towards the camera
  vec4 UseNormal;         // x below 0.9: flat brightness; otherwise lit by the facing normal
  vec4 LightVector;
} vc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inLocalWorld0;
layout(location = 4) in vec4 inLocalWorld1;
layout(location = 5) in vec4 inLocalWorld2;
layout(location = 6) in vec4 inLocalWorld3;
layout(location = 7) in vec4 inView0;
layout(location = 8) in vec4 inView1;
layout(location = 9) in vec4 inView2;
layout(location = 10) in vec4 inView3;

layout(location = 0) out vec4 outUv;
layout(location = 1) out vec4 outScreen;     // clip x, minus clip y, clip z, clip w
layout(location = 2) out vec4 outViewPos;
layout(location = 3) out vec4 outColor;

void main() {
  vec4 local = vec4(inPosition, 1.0);
  vec4 world = vec4(dot(inLocalWorld0, local), dot(inLocalWorld1, local), dot(inLocalWorld2, local), dot(inLocalWorld3, local));
  vec4 clip = vec4(dot(vc.ViewProj[0], world), dot(vc.ViewProj[1], world), dot(vc.ViewProj[2], world), dot(vc.ViewProj[3], world));

  float brightness = 1.0;
  if (vc.UseNormal.x >= 0.9) {
    vec3 toEye = fx_normalize(inPosition - vc.ViewProj[3].xyz);
    vec3 normal = inLocalWorld2.xyz;
    if (dot(toEye, normal) < 0.0) normal = -normal;
    vec3 lightDir = vc.LightVector.xyz * inversesqrt(dot(vc.LightVector, vc.LightVector));
    brightness = clamp(2.0 * max(dot(lightDir, normal), 0.1), 0.0, 1.0);
  }

  vec4 viewPos = vec4(dot(inView0, local), dot(inView1, local), dot(inView2, local), dot(inView3, local));
  // A sprite lying flat (its second axis pointing straight down) reports a view depth of zero.
  if (-inLocalWorld1.z > 0.999) viewPos.z = 0.0;

  gl_Position = vec4(clip.xy, clip.z - vc.ZOffset.x, clip.w);
  outUv = vec4(inUv, 0.0, 0.0);
  outScreen = vec4(clip.x, -clip.y, clip.z, clip.w);
  outViewPos = viewPos;
  outColor = vec4(inColor.rgb * brightness, inColor.a);
}
