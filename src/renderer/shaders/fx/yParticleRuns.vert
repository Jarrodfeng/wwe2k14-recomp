#version 450
// Particle runs (Session 101, lane 67; docs\renderer\PARTICLES_PROMOTION_MAP.md section 4): the yParticle vertex stage for a RUN of particles drawn as one draw.
// Our own code, written from the arithmetic yParticle.vert already holds. The only difference: the particle's LocalWorld rows (the game's c228-c231, which change
// from one particle to the next) arrive per vertex, in the merged vertex buffer (src\renderer_adapter\particle_runs.h: 32 bytes of the game's vertex, then 4 x vec4),
// instead of from the constant block. Everything else (ViewProj, ZOffset, UseNormal, LightVector) is the run's, the same for every particle of it.
// Outputs are the yParticle ones, so the game's own pixel stage (yParticle / base, or any stage reading the same three interpolators) draws it.
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
layout(location = 3) in vec2 inUv2;
layout(location = 4) in vec4 inLocalWorld0;
layout(location = 5) in vec4 inLocalWorld1;
layout(location = 6) in vec4 inLocalWorld2;
layout(location = 7) in vec4 inLocalWorld3;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec2 outUv2;
layout(location = 2) out vec4 outColor;

void main() {
  vec4 local = vec4(inPosition, 1.0);
  vec4 world = vec4(dot(inLocalWorld0, local), dot(inLocalWorld1, local), dot(inLocalWorld2, local), dot(inLocalWorld3, local));
  gl_Position = vec4(dot(vc.ViewProj[0], world), dot(vc.ViewProj[1], world), dot(vc.ViewProj[2], world) - vc.ZOffset.x, dot(vc.ViewProj[3], world));

  float brightness = 1.0;
  if (vc.UseNormal.x >= 0.9) {
    // The sprite's facing normal is the third world axis, flipped to face the eye; the eye direction is taken from the object position and the view matrix's translation row.
    vec3 toEye = fx_normalize(inPosition - vc.ViewProj[3].xyz);
    vec3 normal = inLocalWorld2.xyz;
    if (dot(toEye, normal) < 0.0) normal = -normal;
    vec3 lightDir = vc.LightVector.xyz * inversesqrt(dot(vc.LightVector, vc.LightVector));
    brightness = clamp(2.0 * max(dot(lightDir, normal), 0.1), 0.0, 1.0);
  }
  outUv = inUv;
  outUv2 = inUv2;
  outColor = vec4(inColor.rgb * brightness, inColor.a);
}
