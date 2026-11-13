#version 450
// Lit, textured surface that is hidden where the camera flash point is behind something (vertex stage). See docs\renderer\effects\yFlashDefault.md.
// Options: FX_SKINNED (1-4 bones chosen by the bool constants, otherwise one bone), FX_SHADOW (also hands on the shadow-map position and the direction and depth toward the light).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrixArray[63];
  vec4 EyePos;
  vec4 FlashPoint;          // the flash's position, tested against the depth buffer
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 MdlAlpha;
  vec4 Min;                 // the lowest value each light's N.L is clamped to
  uvec4 Bools[2];
#ifdef FX_SHADOW
  vec4 TexProjLight[4];
  vec4 LightPos;            // row 2 of ViewLight: the light's position
  vec4 LightDepth[2];       // rows 2 and 3 of ViewProjLight: depth and w in the light's view
#endif
} vc;

#define CH_BONES vc.WorldMatrixArray
#include "fx_character.glsl"

layout(set = 1, binding = 1) uniform sampler2D textures[4];   // textures[1] is the game's vertex texture slot 17: the depth buffer

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
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outView;
layout(location = 3) out vec4 outShadow;
layout(location = 4) out vec4 outToLight;
layout(location = 5) out vec4 outLightDepth;
layout(location = 6) out vec4 outColor;

void main() {
  vec3 world;
  vec3 normal;
#ifdef FX_SKINNED
  // The game sets exactly one of the bools: Bools[1] = two bones, Bools[2] = three, Bools[3] = four, none = one.
  // Every bone but the last takes its stored weight; the last takes what is left of 1.
  int boneCount = fx_bool(vc.Bools, 1) ? 2 : (fx_bool(vc.Bools, 2) ? 3 : (fx_bool(vc.Bools, 3) ? 4 : 1));
  float ids[4] = float[4](inBoneIds.x, inBoneIds.y, inBoneIds.z, inBoneIds.w);
  float weights[3] = float[3](inWeights.x, inWeights.y, inWeights.z);
  float rest = 1.0;
  world = vec3(0.0);
  normal = vec3(0.0);
  for (int i = 0; i < 4; ++i) {
    if (i >= boneCount) break;
    int b = ch_bone(ids[i]);
    float w = (i == boneCount - 1) ? rest : weights[i];
    world += ch_bone_point(b, inPosition) * w;
    normal += ch_bone_turn(b, inNormal) * w;
    if (i < 3) rest -= weights[i];
  }
#else
  int bone = fx_bool(vc.Bools, 0) ? ch_bone(inBoneId) : 0;
  world = ch_bone_point(bone, inPosition);
  normal = ch_bone_turn(bone, inNormal);
#endif
  vec4 w4 = vec4(world, 1.0);
  gl_Position = vec4(dot(vc.ViewProj[0], w4), dot(vc.ViewProj[1], w4), dot(vc.ViewProj[2], w4), dot(vc.ViewProj[3], w4));

  // Is the flash point hidden behind the scene? Look its screen position up in the depth buffer.
  vec4 flash = vec4(dot(vc.ViewProj[0], vc.FlashPoint), dot(vc.ViewProj[1], vc.FlashPoint), dot(vc.ViewProj[2], vc.FlashPoint), dot(vc.ViewProj[3], vc.FlashPoint));
  float inverseW = 1.0 / flash.w;
  vec2 screen = vec2(0.5 * flash.x * inverseW + 0.5, 1.0 - (0.5 * flash.y * inverseW + 0.5));
  float flashDepth = textureLod(textures[1], screen, 0.0).x;
#ifdef FX_SKINNED
  // The skinned build of this shader reads the depth buffer but its test can never fire, so the surface is never hidden.
  bool hidden = false;
#else
  bool hidden = flashDepth > 1.0 - flash.z * inverseW;
#endif

  vec3 n = fx_normalize(normal);
  float hemisphere = 0.5 - 0.5 * dot(vc.SkyDir.xyz, n);
  vec3 ambient = mix(vc.SkyCol.xyz, vc.GroundCol.xyz, hemisphere) * vc.MatAmbCol.xyz;
  vec3 diffuse = vec3(0.0);
  for (int i = 0; i < 4; ++i) diffuse += max(dot(vc.DifLghVec[i].xyz, n), vc.Min[i]) * vc.DifLghCol[i].xyz;

  outUv = vec4(inUv, 0.0, 0.0);
  outNormal = vec4(-n, 0.0);
  outView = vec4(vc.EyePos.xyz - world, 0.0);
#ifdef FX_SHADOW
  outShadow = vec4(dot(vc.TexProjLight[0], w4), dot(vc.TexProjLight[1], w4), dot(vc.TexProjLight[2], w4), dot(vc.TexProjLight[3], w4));
  outToLight = vec4(fx_normalize(vec3(vc.LightPos.x - world.x, world.y - vc.LightPos.y, vc.LightPos.z - world.z)), 0.0);
  outLightDepth = vec4(dot(vc.LightDepth[0], w4), dot(vc.LightDepth[1], w4), 0.0, 0.0);
#else
  outShadow = vec4(0.0);
  outToLight = vec4(0.0);
  outLightDepth = vec4(0.0);
#endif
  float alpha = hidden ? 0.0 : vc.MatAmbCol.w * vc.MatDifCol.w * vc.MdlAlpha.x;
  outColor = vec4(diffuse * vc.MatDifCol.xyz + ambient, alpha) * inColor;
}
