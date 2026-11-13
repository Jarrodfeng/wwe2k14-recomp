#version 450
// Default lit model (props, ring gear, arena objects), vertex stage. See docs\renderer\effects\yDefault.md.
// Options: FX_SKINNED (2-4 bones chosen by the bool constants, otherwise one bone), FX_SHADOW (writes the shadow-map position, light direction and depth).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrixArray[63];
  vec4 EyePos;
  vec4 LightMin;         // the floor of each of the four lights' diffuse term
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 MdlAlpha;
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
  uvec4 Bools[2];
#ifdef FX_SHADOW
  vec4 ViewProjLightDepth[2];   // rows 2 and 3 of ViewProjLight
  vec4 TexProjLight[4];
  vec4 LightEye;                // ViewLight row 2: the point the light direction is measured from
#endif
} vc;

#define CH_BONES vc.WorldMatrixArray
#include "fx_character.glsl"

#ifdef FX_SKINNED
// Blend across 1-4 bones; the last bone in use takes what is left of 1. The normal follows all of them.
void skin(vec4 position, vec3 normal, vec3 w, vec4 ids, int boneCount, out vec3 worldPos, out vec3 worldNormal) {
  vec4 id = vec4(ids);
  float rest = 1.0;
  worldPos = vec3(0.0);
  worldNormal = vec3(0.0);
  for (int i = 0; i < 4; ++i) {
    if (i >= boneCount) break;
    float weight = (i == boneCount - 1) ? rest : w[i];
    rest -= weight;
    int b = ch_bone(id[i]);
    worldPos += ch_bone_point(b, position) * weight;
    worldNormal += ch_bone_turn(b, normal) * weight;
  }
}
#endif

layout(location = 0) in vec4 inPosition;
#ifdef FX_SKINNED
layout(location = 1) in vec3 inWeights;
layout(location = 2) in vec4 inBoneIds;
#else
layout(location = 2) in float inBoneId;
#endif
layout(location = 3) in vec3 inNormal;
layout(location = 5) in vec4 inColor;
layout(location = 4) in vec2 inUv;

layout(location = 0) out vec4 outUv;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outView;
layout(location = 3) out vec4 outShadow;
layout(location = 4) out vec4 outLightDir;
layout(location = 5) out vec4 outDepth;
layout(location = 6) out vec4 outLit;

void main() {
  vec3 position;
  vec3 normal;
#ifdef FX_SKINNED
  int boneCount = fx_bool(vc.Bools, 1) ? 2 : (fx_bool(vc.Bools, 2) ? 3 : (fx_bool(vc.Bools, 3) ? 4 : 1));
  skin(inPosition, inNormal, inWeights, inBoneIds, boneCount, position, normal);
#else
  int bone = fx_bool(vc.Bools, 0) ? ch_bone(inBoneId) : 0;
  position = ch_bone_point(bone, inPosition);
  normal = ch_bone_turn(bone, inNormal);
#endif
  vec4 world = vec4(position, 1.0);
  gl_Position = vec4(dot(vc.ViewProj[0], world), dot(vc.ViewProj[1], world), dot(vc.ViewProj[2], world), dot(vc.ViewProj[3], world));

  vec3 n = fx_normalize(normal);
  // Four directional lights, each floored, plus a sky-to-ground hemisphere on the ambient colour.
  vec3 diffuse = vec3(0.0);
  for (int i = 0; i < 4; ++i) {
    diffuse += max(dot(n, vc.DifLghVec[i].xyz), vc.LightMin[i]) * vc.DifLghCol[i].xyz;
  }
  float skyT = 0.5 - 0.5 * dot(n, vc.SkyDir.xyz);
  vec3 ambient = mix(vc.SkyCol.xyz, vc.GroundCol.xyz, skyT) * vc.MatAmbCol.xyz;

  outUv = vec4(inUv, 0.0, 0.0);
  outNormal = vec4(-n, 0.0);
  outView = vec4(vc.EyePos.xyz - position, 0.0);
#ifdef FX_SHADOW
  outShadow = vec4(dot(vc.TexProjLight[0], world), dot(vc.TexProjLight[1], world), dot(vc.TexProjLight[2], world), dot(vc.TexProjLight[3], world));
  vec3 toLight = vec3(vc.LightEye.x - position.x, position.y - vc.LightEye.y, vc.LightEye.z - position.z);
  outLightDir = vec4(fx_normalize(toLight), 0.0);
  outDepth = vec4(dot(vc.ViewProjLightDepth[0], world), dot(vc.ViewProjLightDepth[1], world), 0.0, 0.0);
#else
  outShadow = vec4(0.0);
  outLightDir = vec4(0.0);
  outDepth = vec4(0.0);
#endif
  outLit = vec4(diffuse * vc.MatDifCol.xyz + ambient, vc.MdlAlpha.x * vc.MatAmbCol.w * vc.MatDifCol.w) * inColor;
}
