#version 450
// Transparent-capable character (wrestler) skin and cloth, vertex stage. See docs\renderer\effects\yCh_trans.md.
// Same vertex work as yCh_Ls, plus the view-space normal for the environment cube lookups.
// Options: FX_SKINNED (1-4 bones chosen by the bool constants, otherwise one rigid bone), FX_SHADOW (writes the shadow-map position),
//          FX_SCREEN_CONVERT (re-centres and rescales the clip position with ConvScreen), FX_VIEW_NORMAL (writes the normal in view space).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrixArray[63];
  vec4 EyePos;
  vec4 FogDistance;       // x start, y end, z 1 / (end - start)
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 NormalFlip;        // w: 0 normal as is, -2 flipped
  vec4 Param0;            // x specular gain, y Fresnel power
  vec4 RimLightDepth;
  vec4 RimLightParam3;
  vec4 MdlAlpha;
  vec4 KeyLightColor;     // the fifth light (DifLghCol[4]), the one this stage shades with
  vec4 KeyLightVec;       // DifLghVec[4]
#ifdef FX_SKINNED
  uvec4 Bools[2];
#endif
#ifdef FX_SHADOW
  vec4 TexProjLightXY[2]; // rows 0 and 1 of TexProjLight
  vec4 TexProjLightW;     // row 3
  vec4 ViewLightDepth;    // row 2 of ViewLight
#endif
#ifdef FX_SCREEN_CONVERT
  vec4 ConvScreen;
#endif
#ifdef FX_VIEW_NORMAL
  vec4 View[3];
#endif
} vc;

#define CH_BONES vc.WorldMatrixArray
#include "fx_character.glsl"

layout(location = 0) in vec4 inPosition;
#ifdef FX_SKINNED
layout(location = 1) in vec3 inWeights;
layout(location = 2) in vec4 inBoneIds;
#else
layout(location = 2) in float inBoneId;
#endif
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec2 inUv;

layout(location = 0) out vec4 outNormal;
layout(location = 1) out vec4 outUvFog;
layout(location = 2) out vec4 outView;
layout(location = 3) out vec4 outShadow;
layout(location = 4) out vec4 outUnused;
layout(location = 5) out vec4 outRim;
layout(location = 6) out vec4 outTint;
layout(location = 7) out vec4 outSpecular;
#ifdef FX_VIEW_NORMAL
layout(location = 8) out vec4 outViewNormal;
#endif

// 0 at from, 1 at to, clamped: the depth ramps of the rim light.
float ramp(float z, float from, float to) { return clamp((z - from) / (to - from), 0.0, 1.0); }

void main() {
  vec3 normalIn = inNormal * (1.0 + vc.NormalFlip.w);
  vec3 position;
  vec3 normal;
#ifdef FX_SKINNED
  int boneCount = fx_bool(vc.Bools, 3) ? 4 : (fx_bool(vc.Bools, 2) ? 3 : (fx_bool(vc.Bools, 1) ? 2 : 1));
  ch_skin(inPosition, normalIn, inWeights, inBoneIds, boneCount, position, normal);
#else
  int bone = ch_bone(inBoneId);
  position = ch_bone_point(bone, inPosition);
  normal = ch_bone_turn(bone, normalIn);
#endif

  vec4 world = vec4(position, 1.0);
  vec4 clip = vec4(dot(vc.ViewProj[0], world), dot(vc.ViewProj[1], world), dot(vc.ViewProj[2], world), dot(vc.ViewProj[3], world));

  vec3 n = fx_normalize(normal);
  vec3 v = fx_normalize(vc.EyePos.xyz - position);
  float keyHighlight = max(dot(fx_normalize(v + n), vc.KeyLightVec.xyz), 0.0);
  float fresnel = fx_pow(1.0 - abs(dot(v, n)), vc.Param0.y);

  vec2 ndc = clip.w != 0.0 ? clip.xy / clip.w : vec2(0.0);

  float fogDepth = min(max(clip.w, vc.FogDistance.x), vc.FogDistance.y);

  outNormal = vec4(n, 0.0);
  outUvFog = vec4(inUv, (fogDepth - vc.FogDistance.x) * vc.FogDistance.z, 0.0);
  outView = vec4(v, 1.0);
#ifdef FX_SHADOW
  outShadow = vec4(dot(vc.TexProjLightXY[0], world), dot(vc.TexProjLightXY[1], world), dot(vc.ViewLightDepth, world), dot(vc.TexProjLightW, world));
#else
  outShadow = vec4(0.0);
#endif
  outUnused = vec4(0.0);
  // xy: where this vertex is on screen (0..1, y down), to read the screen-space rim map; z: rim depth fade; w: rim strength by distance.
  float rimStrength = mix(vc.RimLightParam3.z, vc.RimLightParam3.w, ramp(clip.w, vc.RimLightParam3.x, vc.RimLightParam3.y));
  outRim = vec4(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5, ramp(clip.w, vc.RimLightDepth.x, vc.RimLightDepth.y), rimStrength);
  outTint = vec4(1.0, 1.0, 1.0, vc.MdlAlpha.x * vc.MatAmbCol.w * vc.MatDifCol.w);
  outSpecular = vec4(vc.KeyLightColor.xyz * keyHighlight * fresnel * clip.w * vc.Param0.x, 0.0);
#ifdef FX_VIEW_NORMAL
  outViewNormal = vec4(dot(n, vc.View[0].xyz), dot(n, vc.View[1].xyz), dot(n, vc.View[2].xyz), 0.0);
#endif

#ifdef FX_SCREEN_CONVERT
  clip.xy = (ndc - vc.ConvScreen.xy) * vc.ConvScreen.w * clip.w;
#endif
  gl_Position = clip;
}
