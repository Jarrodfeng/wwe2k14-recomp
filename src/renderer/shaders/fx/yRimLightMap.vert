#version 450
// Shadow-map depth pass, vertex stage. See docs\renderer\effects\yShadowMap.md.
// Options: FX_SKINNED (1-4 bones chosen by the bool constants), FX_SINGLE_BONE (one bone picked by a per-vertex index, or bone 0 when the bool is off).
//          Neither: the vertices are already in world space.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 RimLightDepth;       // x depth where the rim map starts, y where it ends
#if defined(FX_SKINNED) || defined(FX_SINGLE_BONE)
  vec4 WorldMatrixArray[63];
  uvec4 Bools[2];
#endif
} vc;

#if defined(FX_SKINNED) || defined(FX_SINGLE_BONE)
#define CH_BONES vc.WorldMatrixArray
#include "fx_character.glsl"
layout(location = 0) in vec4 inPosition;
#else
layout(location = 0) in vec3 inPosition;
#endif
#ifdef FX_SKINNED
layout(location = 1) in vec3 inWeights;
layout(location = 2) in vec4 inBoneIds;
#endif
#ifdef FX_SINGLE_BONE
layout(location = 1) in float inBoneId;
#endif

layout(location = 0) out vec4 outDepth;

void main() {
  vec3 world;
#ifdef FX_SKINNED
  int boneCount = fx_bool(vc.Bools, 3) ? 4 : (fx_bool(vc.Bools, 2) ? 3 : (fx_bool(vc.Bools, 1) ? 2 : 1));
  vec3 unusedNormal;
  ch_skin(inPosition, vec3(0.0), inWeights, inBoneIds, boneCount, world, unusedNormal);
#elif defined(FX_SINGLE_BONE)
  world = ch_bone_point(fx_bool(vc.Bools, 0) ? ch_bone(inBoneId) : 0, inPosition);
#else
  world = inPosition;
#endif
  vec4 world4 = vec4(world, 1.0);
  vec4 clip = vec4(dot(vc.ViewProj[0], world4), dot(vc.ViewProj[1], world4), dot(vc.ViewProj[2], world4), dot(vc.ViewProj[3], world4));
  gl_Position = clip;
  // 0 at the near depth, 1 at the far depth, clamped.
  outDepth = vec4(clamp((clip.w - vc.RimLightDepth.x) / (vc.RimLightDepth.y - vc.RimLightDepth.x), 0.0, 1.0), 0.0, 0.0, 0.0);
}
