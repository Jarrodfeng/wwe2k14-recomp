#version 450
// Depth pre-pass, vertex stage: positions only. See docs\renderer\effects\yZPass.md.
// Options: FX_SINGLE_BONE (one bone picked by a per-vertex index, or bone 0 when the bool is off), FX_MULTI_BONE (2 to 5 bones blended by weights, the count
//          chosen by the bool constants, highest first). Without either the vertices are already in world space.
//          FX_MULTI_BONE (2 to 5 bones blended by weights, the count chosen by the bool constants, highest first).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
#if defined(FX_SINGLE_BONE) || defined(FX_MULTI_BONE)
  vec4 WorldMatrixArray[63];
  uvec4 Bools[2];
#endif
} vc;

#if defined(FX_SINGLE_BONE) || defined(FX_MULTI_BONE)
#define CH_BONES vc.WorldMatrixArray
#include "fx_character.glsl"
layout(location = 0) in vec4 inPosition;
#endif
#ifdef FX_SINGLE_BONE
layout(location = 1) in float inBoneId;
#elif defined(FX_MULTI_BONE)
layout(location = 2) in vec4 inWeights;
layout(location = 3) in vec4 inBoneIds;
layout(location = 4) in float inFifthBoneId;
#else
layout(location = 0) in vec3 inPosition;
#endif

void main() {
#ifdef FX_MULTI_BONE
  // Bone i pairs with weight i; a fifth bone has its own id and takes what is left of the weights.
  int count = fx_bool(vc.Bools, 4) ? 5 : (fx_bool(vc.Bools, 3) ? 4 : (fx_bool(vc.Bools, 2) ? 3 : (fx_bool(vc.Bools, 1) ? 2 : 1)));
  float ids[5] = float[5](inBoneIds.x, inBoneIds.y, inBoneIds.z, inBoneIds.w, inFifthBoneId);
  float weights[4] = float[4](inWeights.x, inWeights.y, inWeights.z, inWeights.w);
  vec3 world = vec3(0.0);
  float rest = 1.0;
  for (int i = 0; i < 5; ++i) {
    if (i >= count) break;
    float weight = (i == count - 1) ? rest : weights[i];
    rest -= weight;
    // With five bones, a zero fourth weight means the vertex has only four: the fifth is dropped.
    if (i == 4 && inWeights.w == 0.0) weight = 0.0;
    world += ch_bone_point(ch_bone(ids[i]), inPosition) * weight;
  }
#elif defined(FX_SINGLE_BONE)
  vec3 world = ch_bone_point(fx_bool(vc.Bools, 0) ? ch_bone(inBoneId) : 0, inPosition);
#else
  vec3 world = inPosition;
#endif
  vec4 p = vec4(world, 1.0);
  gl_Position = vec4(dot(vc.ViewProj[0], p), dot(vc.ViewProj[1], p), dot(vc.ViewProj[2], p), dot(vc.ViewProj[3], p));
}
