#version 450
// Crowd members that are skinned and carry a vertex colour (the animated audience), vertex stage. See docs\renderer\effects\yAudDefault.md.
// Options: FX_SKINNED (2-4 bones chosen by the bool constants, otherwise one rigid bone), FX_LIT (vertex lighting from the four lights and the sky/ground ambient),
//          FX_FRAME (outputs the normal and a tangent frame for a normal map), FX_EYE (frame variants: also the vector to the eye),
//          FX_LM_VERTEX (reads the baked light map here, in the vertex stage), FX_LM_UV (outputs the light map coordinate for the pixel stage).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrixArray[63];
  uvec4 Bools[2];
#if defined(FX_LM_VERTEX) || defined(FX_LM_UV)
  vec4 LightMapParam;       // xy world-to-light-map scale, z brightness
#endif
#ifdef FX_EYE
  vec4 EyePos;
#endif
#ifdef FX_LIT
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 LightMin;            // the floor of each light's facing term: x light 0 ... w light 3
#endif
} vc;

#define CH_BONES vc.WorldMatrixArray
#include "fx_character.glsl"

layout(set = 1, binding = 1) uniform sampler2D textures[4];

layout(location = 0) in vec4 inPosition;
#ifdef FX_SKINNED
layout(location = 1) in vec3 inWeights;
layout(location = 2) in vec4 inBoneIds;
#else
layout(location = 2) in float inBoneId;
#endif
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inColor;
layout(location = 5) in vec2 inUv;

layout(location = 0) out vec4 outUv;
layout(location = 1) out vec4 outShade;
#ifdef FX_FRAME
layout(location = 2) out vec4 outTangent;
layout(location = 3) out vec4 outBitangent;
layout(location = 4) out vec4 outSlotA;
#ifdef FX_EYE
layout(location = 5) out vec4 outSlotB;
#endif
#endif

#ifdef FX_LIT
// Four directional lights plus a sky-to-ground ambient, for a world-space normal.
vec3 audience_lighting(vec3 n) {
  vec3 diffuse = vec3(0.0);
  for (int i = 0; i < 4; ++i) diffuse += max(dot(n, -vc.DifLghVec[i].xyz), vc.LightMin[i]) * vc.DifLghCol[i].xyz;
  vec3 ambient = mix(vc.SkyCol.xyz, vc.GroundCol.xyz, 0.5 * dot(n, vc.SkyDir.xyz) + 0.5) * vc.MatAmbCol.xyz;
  return diffuse * vc.MatDifCol.xyz + ambient;
}
#endif

void main() {
  vec3 world;
  vec3 skinnedNormal;
#ifdef FX_SKINNED
  // The bool constants say how many bones the mesh uses; exactly one of them is set.
  int boneCount = fx_bool(vc.Bools, 1) ? 2 : (fx_bool(vc.Bools, 2) ? 3 : 4);
  ch_skin(inPosition, inNormal, inWeights, inBoneIds, boneCount, world, skinnedNormal);
  if (boneCount == 4) {
    // Unlike the wrestlers, the crowd's four-bone normal is blended over all four bones like the position.
    skinnedNormal = ch_bone_turn(ch_bone(inBoneIds.x), inNormal) * inWeights.x + ch_bone_turn(ch_bone(inBoneIds.y), inNormal) * inWeights.y
                  + ch_bone_turn(ch_bone(inBoneIds.z), inNormal) * inWeights.z
                  + ch_bone_turn(ch_bone(inBoneIds.w), inNormal) * (1.0 - inWeights.x - inWeights.y - inWeights.z);
  }
#else
  // One rigid bone: the mesh's own bone id when the bool is set, otherwise the first matrix.
  int bone = fx_bool(vc.Bools, 0) ? ch_bone(inBoneId) : 0;
  world = ch_bone_point(bone, inPosition);
  skinnedNormal = ch_bone_turn(bone, inNormal);
#endif
  vec4 world4 = vec4(world, 1.0);
  gl_Position = vec4(dot(vc.ViewProj[0], world4), dot(vc.ViewProj[1], world4), dot(vc.ViewProj[2], world4), dot(vc.ViewProj[3], world4));

  // The baked light map is laid out over the ground plane: world x and z.
  vec4 lightMap = vec4(1.0);
#if defined(FX_LM_VERTEX) || defined(FX_LM_UV)
  vec2 lightMapUv = (world.xz * 0.5) * vc.LightMapParam.xy + 0.5;
#endif
#ifdef FX_LM_VERTEX
  lightMap = textureLod(textures[0], lightMapUv, 0.0) * vc.LightMapParam.z;
#endif
  outUv = vec4(inUv, 0.0, 0.0);
#ifdef FX_LM_UV
  outUv.zw = lightMapUv;
#endif

  // The world normal comes out negated, and is not renormalised.
  vec3 n = -skinnedNormal;

#ifdef FX_FRAME
  // Tangent frame built from the normal: a vertical normal (|y| >= 0.99) takes the x axis as its reference, any other the y axis.
  vec3 tangent, bitangent;
  if (abs(n.y) >= 0.99) {
    float inv = inversesqrt(n.y * n.y + n.z * n.z);
    tangent = vec3((n.y * n.y + n.z * n.z) * inv, -n.x * n.y * inv, -n.x * n.z * inv);
    bitangent = vec3(0.0, -n.z * inv, n.y * inv);
  } else {
    float flatLenSq = n.x * n.x + n.z * n.z;
    float inv = flatLenSq > 0.0 ? inversesqrt(flatLenSq) : 0.0;   // a normal pointing straight along y has no flat part
    tangent = vec3(-n.z * inv, 0.0, n.x * inv);
    bitangent = vec3(n.x * n.y * inv, -(n.x * n.x + n.z * n.z) * inv, n.y * n.z * inv);
  }
  outShade = vec4(n, 0.0);
  outTangent = vec4(tangent, 0.0);
  outBitangent = vec4(bitangent, 0.0);
#ifdef FX_EYE
  outSlotA = vec4(vc.EyePos.xyz - world, 0.0);
  outSlotB = lightMap * inColor;
#else
  outSlotA = lightMap * inColor;
#endif
#else
  vec3 shade = vec3(1.0);
#ifdef FX_LIT
  shade = audience_lighting(n);
#endif
  outShade = vec4(shade * lightMap.rgb * inColor.rgb, lightMap.a * inColor.a);
#endif
}
