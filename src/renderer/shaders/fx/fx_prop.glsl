// Shared by the prop surface effects (yProp_*). See docs\renderer\effects\yProp.md.
// Include with FX_PROP_VERT or FX_PROP_FRAG defined, after #version and fx_common.glsl. The effect files define which options exist.
//
// Vertex options: FX_SKINNED (bone-blended; otherwise one world matrix), FX_SPEC (also passes normal and view vector for specular),
//   FX_SCREEN_CONVERT, FX_LIGHTMAP (passes the light-map coordinate), FX_SCROLL (uv moves with time), FX_UV2 / FX_UV3 / FX_UV4 (number of scrolled uv sets).
// Fragment options: FX_SPEC, FX_SPEC_MAP, FX_ALPHA_GLOSS, FX_LOGO (logo texture composited over the diffuse), FX_NOISE, FX_LIGHTMAP, FX_UV2 / FX_UV3 / FX_UV4 (layers blended).
#ifndef FX_PROP_GLSL
#define FX_PROP_GLSL

// ---- Varying locations (plain numbers: the build reads them as literals): uv sets, [noise uv], [normal, view], [light-map uv], colour.
#if defined(FX_UV4)
#define FP_UVS 4
#elif defined(FX_UV3)
#define FP_UVS 3
#elif defined(FX_UV2)
#define FP_UVS 2
#else
#define FP_UVS 1
#endif
#if FP_UVS == 1 && !defined(FX_NOISE) && !defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 1
#define FP_VIEW_LOC 0
#define FP_LM_LOC 0
#define FP_COLOR_LOC 1
#elif FP_UVS == 1 && !defined(FX_NOISE) && !defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 1
#define FP_VIEW_LOC 0
#define FP_LM_LOC 1
#define FP_COLOR_LOC 2
#elif FP_UVS == 1 && !defined(FX_NOISE) && defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 1
#define FP_VIEW_LOC 2
#define FP_LM_LOC 0
#define FP_COLOR_LOC 3
#elif FP_UVS == 1 && !defined(FX_NOISE) && defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 1
#define FP_VIEW_LOC 2
#define FP_LM_LOC 3
#define FP_COLOR_LOC 4
#elif FP_UVS == 1 && defined(FX_NOISE) && !defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 1
#define FP_NORMAL_LOC 2
#define FP_VIEW_LOC 0
#define FP_LM_LOC 0
#define FP_COLOR_LOC 2
#elif FP_UVS == 1 && defined(FX_NOISE) && !defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 1
#define FP_NORMAL_LOC 2
#define FP_VIEW_LOC 0
#define FP_LM_LOC 2
#define FP_COLOR_LOC 3
#elif FP_UVS == 1 && defined(FX_NOISE) && defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 1
#define FP_NORMAL_LOC 2
#define FP_VIEW_LOC 3
#define FP_LM_LOC 0
#define FP_COLOR_LOC 4
#elif FP_UVS == 1 && defined(FX_NOISE) && defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 1
#define FP_NORMAL_LOC 2
#define FP_VIEW_LOC 3
#define FP_LM_LOC 4
#define FP_COLOR_LOC 5
#elif FP_UVS == 2 && !defined(FX_NOISE) && !defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 2
#define FP_VIEW_LOC 0
#define FP_LM_LOC 0
#define FP_COLOR_LOC 2
#elif FP_UVS == 2 && !defined(FX_NOISE) && !defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 2
#define FP_VIEW_LOC 0
#define FP_LM_LOC 2
#define FP_COLOR_LOC 3
#elif FP_UVS == 2 && !defined(FX_NOISE) && defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 2
#define FP_VIEW_LOC 3
#define FP_LM_LOC 0
#define FP_COLOR_LOC 4
#elif FP_UVS == 2 && !defined(FX_NOISE) && defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 2
#define FP_VIEW_LOC 3
#define FP_LM_LOC 4
#define FP_COLOR_LOC 5
#elif FP_UVS == 2 && defined(FX_NOISE) && !defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 2
#define FP_NORMAL_LOC 3
#define FP_VIEW_LOC 0
#define FP_LM_LOC 0
#define FP_COLOR_LOC 3
#elif FP_UVS == 2 && defined(FX_NOISE) && !defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 2
#define FP_NORMAL_LOC 3
#define FP_VIEW_LOC 0
#define FP_LM_LOC 3
#define FP_COLOR_LOC 4
#elif FP_UVS == 2 && defined(FX_NOISE) && defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 2
#define FP_NORMAL_LOC 3
#define FP_VIEW_LOC 4
#define FP_LM_LOC 0
#define FP_COLOR_LOC 5
#elif FP_UVS == 2 && defined(FX_NOISE) && defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 2
#define FP_NORMAL_LOC 3
#define FP_VIEW_LOC 4
#define FP_LM_LOC 5
#define FP_COLOR_LOC 6
#elif FP_UVS == 3 && !defined(FX_NOISE) && !defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 3
#define FP_VIEW_LOC 0
#define FP_LM_LOC 0
#define FP_COLOR_LOC 3
#elif FP_UVS == 3 && !defined(FX_NOISE) && !defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 3
#define FP_VIEW_LOC 0
#define FP_LM_LOC 3
#define FP_COLOR_LOC 4
#elif FP_UVS == 3 && !defined(FX_NOISE) && defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 3
#define FP_VIEW_LOC 4
#define FP_LM_LOC 0
#define FP_COLOR_LOC 5
#elif FP_UVS == 3 && !defined(FX_NOISE) && defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 3
#define FP_VIEW_LOC 4
#define FP_LM_LOC 5
#define FP_COLOR_LOC 6
#elif FP_UVS == 3 && defined(FX_NOISE) && !defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 3
#define FP_NORMAL_LOC 4
#define FP_VIEW_LOC 0
#define FP_LM_LOC 0
#define FP_COLOR_LOC 4
#elif FP_UVS == 3 && defined(FX_NOISE) && !defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 3
#define FP_NORMAL_LOC 4
#define FP_VIEW_LOC 0
#define FP_LM_LOC 4
#define FP_COLOR_LOC 5
#elif FP_UVS == 3 && defined(FX_NOISE) && defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 3
#define FP_NORMAL_LOC 4
#define FP_VIEW_LOC 5
#define FP_LM_LOC 0
#define FP_COLOR_LOC 6
#elif FP_UVS == 3 && defined(FX_NOISE) && defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 3
#define FP_NORMAL_LOC 4
#define FP_VIEW_LOC 5
#define FP_LM_LOC 6
#define FP_COLOR_LOC 7
#elif FP_UVS == 4 && !defined(FX_NOISE) && !defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 4
#define FP_VIEW_LOC 0
#define FP_LM_LOC 0
#define FP_COLOR_LOC 4
#elif FP_UVS == 4 && !defined(FX_NOISE) && !defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 4
#define FP_VIEW_LOC 0
#define FP_LM_LOC 4
#define FP_COLOR_LOC 5
#elif FP_UVS == 4 && !defined(FX_NOISE) && defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 4
#define FP_VIEW_LOC 5
#define FP_LM_LOC 0
#define FP_COLOR_LOC 6
#elif FP_UVS == 4 && !defined(FX_NOISE) && defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 0
#define FP_NORMAL_LOC 4
#define FP_VIEW_LOC 5
#define FP_LM_LOC 6
#define FP_COLOR_LOC 7
#elif FP_UVS == 4 && defined(FX_NOISE) && !defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 4
#define FP_NORMAL_LOC 5
#define FP_VIEW_LOC 0
#define FP_LM_LOC 0
#define FP_COLOR_LOC 5
#elif FP_UVS == 4 && defined(FX_NOISE) && !defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 4
#define FP_NORMAL_LOC 5
#define FP_VIEW_LOC 0
#define FP_LM_LOC 5
#define FP_COLOR_LOC 6
#elif FP_UVS == 4 && defined(FX_NOISE) && defined(FX_SPEC) && !defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 4
#define FP_NORMAL_LOC 5
#define FP_VIEW_LOC 6
#define FP_LM_LOC 0
#define FP_COLOR_LOC 7
#elif FP_UVS == 4 && defined(FX_NOISE) && defined(FX_SPEC) && defined(FX_LIGHTMAP)
#define FP_NOISE_LOC 4
#define FP_NORMAL_LOC 5
#define FP_VIEW_LOC 6
#define FP_LM_LOC 7
#define FP_COLOR_LOC 8
#endif

#if defined(FX_PROP_VERT) || defined(FX_BUMP_VERT) || defined(FX_PSM_VERT)
// ======================================================================= vertex (FX_BUMP_VERT: the normal-mapped props further below)
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
#ifdef FX_SKINNED
  vec4 WorldMatrixArray[63];
  uvec4 Bools[2];
#else
  vec4 WorldMatrixArray[3];
#endif
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
  vec4 fvMin;             // per-light lower clamp of N.L
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 MdlAlpha;          // x
#if defined(FX_SPEC) || defined(FX_BUMP_VERT)
  vec4 EyePos;
#endif
#ifdef FX_SPEC_FADE
  vec4 SpeLodParam;       // xy: clip depth where the highlight starts to fade out and where it is gone
#endif
#ifdef FX_SCREEN_CONVERT
  vec4 ConvScreen;
#endif
#ifdef FX_LIGHTMAP
  vec4 LightMapParam;     // xy: world xz scale of the light map, z: intensity (used by the pixel stage)
#endif
#if defined(FX_NOISE) || defined(FX_PSM_VERT)
  vec4 NoiseParam;        // xy: world xz scale of the noise texture
#endif
#ifdef FX_PSM_VERT
  vec4 CameraViewProj[4]; // the main camera, for the screen-space shadow lookup
  vec4 PlaneInv[4];       // maps that camera's clip space to the shadow plane's screen
#ifdef FX_SHADOW_LIGHT
  vec4 ShadowPrm[2];      // [1].y: how strongly faces turned away from the shadow light fade
  vec4 TexProjLight[4];   // world to the shadow light's texture space
  vec4 ViewLight[3];      // [2]: the shadow light's view direction
#endif
#endif
#ifdef FX_SCROLL
  vec4 Time;              // x
  vec4 UVScroll0;         // xy: speed of uv set 0, zw: set 1
  vec4 UVScroll1;         // xy: set 2, zw: set 3
#endif
} vc;

#define CH_BONES vc.WorldMatrixArray
#include "fx_character.glsl"

// Position and normal in the world. Bool constants pick 1..5 bones (b4 first, then b0..b3 in order; the game tests them in that order); the fifth is the fixed root matrix taking whatever weight is left.
void prop_skin(vec4 position, vec3 normal, vec4 weights, vec4 ids, out vec3 worldPos, out vec3 worldNormal) {
#ifdef FX_SKINNED
  int boneCount = fx_bool(vc.Bools, 4) ? 5 : (fx_bool(vc.Bools, 0) ? 1 : (fx_bool(vc.Bools, 1) ? 2 : (fx_bool(vc.Bools, 2) ? 3 : (fx_bool(vc.Bools, 3) ? 4 : 0))));
  if (boneCount == 0) {            // no bone count set: the game leaves the vertex as fetched
    worldPos = position.xyz;
    worldNormal = normal;
  } else if (boneCount == 5) {
    vec3 ignoredPos;
    ch_skin(position, normal, weights.xyz, ids, 4, ignoredPos, worldNormal);
    worldPos = ch_bone_point(ch_bone(ids.x), position) * weights.x + ch_bone_point(ch_bone(ids.y), position) * weights.y
             + ch_bone_point(ch_bone(ids.z), position) * weights.z + ch_bone_point(ch_bone(ids.w), position) * weights.w
             + ch_bone_point(0, position) * (1.0 - weights.x - weights.y - weights.z - weights.w);
  } else {
    ch_skin(position, normal, weights.xyz, ids, boneCount, worldPos, worldNormal);
  }
#else
  worldPos = ch_bone_point(0, position);
  worldNormal = ch_bone_turn(0, normal);
#endif
}

// Light the vertex: four directional lights, plus a sky-to-ground ambient. Returns the colour; alpha is the model alpha.
vec4 prop_light(vec3 worldNormal, vec4 vertexColor) {
  vec4 lam = vec4(dot(worldNormal, vc.DifLghVec[0].xyz), dot(worldNormal, vc.DifLghVec[1].xyz), dot(worldNormal, vc.DifLghVec[2].xyz), dot(worldNormal, vc.DifLghVec[3].xyz));
  lam = max(lam, vc.fvMin);
  vec3 diffuse = lam.x * vc.DifLghCol[0].xyz + lam.y * vc.DifLghCol[1].xyz + lam.z * vc.DifLghCol[2].xyz + lam.w * vc.DifLghCol[3].xyz;
  float skyT = 0.5 - dot(worldNormal, vc.SkyDir.xyz) * 0.5;   // 0 where the normal faces SkyDir, 1 against it
  vec3 ambient = mix(vc.SkyCol.xyz, vc.GroundCol.xyz, skyT) * vc.MatAmbCol.xyz;
  float alpha = vc.MdlAlpha.x * (vc.MatAmbCol.w * vc.MatDifCol.w);
  return vec4(diffuse * vc.MatDifCol.xyz + ambient, alpha) * vertexColor;
}

vec4 prop_project(vec3 worldPos) {
  vec4 world = vec4(worldPos, 1.0);
  vec4 clip = vec4(dot(vc.ViewProj[0], world), dot(vc.ViewProj[1], world), dot(vc.ViewProj[2], world), dot(vc.ViewProj[3], world));
#ifdef FX_SCREEN_CONVERT
  vec2 ndc = clip.w != 0.0 ? clip.xy / clip.w : vec2(0.0);   // the game gives zero for a vertex on the eye plane
  clip.xy = (ndc - vc.ConvScreen.xy) * vc.ConvScreen.w * clip.w;
#endif
  return clip;
}

layout(location = 0) in vec4 inPosition;
#ifdef FX_SKINNED
layout(location = 1) in vec4 inWeights;
layout(location = 2) in vec4 inBoneIds;
#endif
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inColor;
layout(location = 5) in vec2 inUv;

#ifdef FX_BUMP_VERT
// Normal-mapped prop: the lighting happens in the pixel stage, so the vertex stage passes the surface frame.
layout(location = 0) out vec4 outUv0;       // xy uv, z highlight strength (1 minus the distance fade)
layout(location = 1) out vec4 outView;      // eye minus position
layout(location = 2) out vec4 outNormal;    // negated world normal
layout(location = 3) out vec4 outTangent;
layout(location = 4) out vec4 outBitangent;
#ifdef FX_LIGHTMAP
layout(location = 5) out vec4 outLightMapUv;
layout(location = 6) out vec4 outColor;
#else
layout(location = 5) out vec4 outColor;
#endif

void main() {
#ifndef FX_SKINNED
  vec4 inWeights = vec4(0.0), inBoneIds = vec4(0.0);
#endif
  vec3 worldPos, n;
  prop_skin(inPosition, inNormal, inWeights, inBoneIds, worldPos, n);
  vec4 clip = prop_project(worldPos);
  gl_Position = clip;

  // A frame from the normal alone: one horizontal axis for mostly-upright normals, another for steep ones.
  vec3 tangent, bitangent;
  if (abs(n.y) < 0.5) {
    tangent = fx_normalize(vec3(n.z, 0.0, -n.x));
    bitangent = cross(tangent, n);
  } else {
    bitangent = fx_normalize(vec3(0.0, n.z, -n.y));
    tangent = cross(bitangent, n);
  }
  float fade = 0.0;
#ifdef FX_SPEC_FADE
  fade = clamp((clip.z - vc.SpeLodParam.x) / (vc.SpeLodParam.y - vc.SpeLodParam.x), 0.0, 1.0);
#endif
  outUv0 = vec4(inUv, 1.0 - fade, 0.0);
  outView = vec4(vc.EyePos.xyz - worldPos, 0.0);
  outNormal = vec4(-n, 0.0);
  outTangent = vec4(tangent, 0.0);
  outBitangent = vec4(bitangent, 0.0);
#ifdef FX_LIGHTMAP
  outLightMapUv = vec4(worldPos.xz * vc.LightMapParam.xy * 0.5 + 0.5, 0.0, 0.0);
#endif
  outColor = vec4(inColor.rgb, inColor.a * (vc.MatAmbCol.w * vc.MatDifCol.w * vc.MdlAlpha.x));
}
#elif defined(FX_PSM_VERT)
// Shadow-receiving prop: lit per vertex like the plain prop, plus a noise uv, a screen-space coordinate for the shadow plane
// and (FX_SHADOW_LIGHT) a projection into the shadow light.
layout(location = 0) out vec4 outUv0;
layout(location = 1) out vec4 outNoiseUv;
layout(location = 2) out vec4 outScreenProj;   // xy * z is the shadow plane's texture coordinate
#ifdef FX_SHADOW_LIGHT
layout(location = 3) out vec4 outLightProj;
layout(location = 4) out vec4 outLightFade;    // z: how much the shadow light faces this vertex
#endif
#ifdef FX_LIGHTMAP
layout(location = 5) out vec4 outLightMapUv;
layout(location = 6) out vec4 outColor;
#else
layout(location = 5) out vec4 outColor;
#endif

void main() {
#ifndef FX_SKINNED
  vec4 inWeights = vec4(0.0), inBoneIds = vec4(0.0);
#endif
  vec3 worldPos, worldNormal;
  prop_skin(inPosition, inNormal, inWeights, inBoneIds, worldPos, worldNormal);
  vec4 clip = prop_project(worldPos);
  gl_Position = clip;
  vec4 world = vec4(worldPos, 1.0);

  outUv0 = vec4(inUv, 0.0, 0.0);
  outNoiseUv = vec4(worldPos.xz * vc.NoiseParam.xy, 0.0, 0.0);
  vec4 cam = vec4(dot(vc.CameraViewProj[0], world), dot(vc.CameraViewProj[1], world), dot(vc.CameraViewProj[2], world), dot(vc.CameraViewProj[3], world));
  vec3 plane = vec3(dot(vc.PlaneInv[0], cam), dot(vc.PlaneInv[1], cam), dot(vc.PlaneInv[3], cam));
  outScreenProj = vec4(vec2(0.5 * plane.x / plane.z + 0.5, 0.5 - 0.5 * plane.y / plane.z) * clip.w, clip.w, 0.0);
#ifdef FX_SHADOW_LIGHT
  outLightProj = vec4(dot(vc.TexProjLight[0], world), dot(vc.TexProjLight[1], world), dot(vc.ViewLight[2], world), dot(vc.TexProjLight[3], world));
  outLightFade = vec4(0.0, 0.0, vc.ShadowPrm[1].y * (1.0 - max(dot(worldNormal, vc.ViewLight[2].xyz), 0.0)), 0.0);
#endif
#ifdef FX_LIGHTMAP
  outLightMapUv = vec4(worldPos.xz * vc.LightMapParam.xy * 0.5 + 0.5, 0.0, 0.0);
#endif
  outColor = prop_light(worldNormal, inColor);
}
#else
layout(location = 0) out vec4 outUv0;
#if FP_UVS >= 2
layout(location = 1) out vec4 outUv1;
#endif
#if FP_UVS >= 3
layout(location = 2) out vec4 outUv2;
#endif
#if FP_UVS >= 4
layout(location = 3) out vec4 outUv3;
#endif
#ifdef FX_NOISE
layout(location = FP_NOISE_LOC) out vec4 outNoiseUv;
#endif
#ifdef FX_SPEC
layout(location = FP_NORMAL_LOC) out vec4 outNormal;
layout(location = FP_VIEW_LOC) out vec4 outView;
#endif
#ifdef FX_LIGHTMAP
layout(location = FP_LM_LOC) out vec4 outLightMapUv;
#endif
layout(location = FP_COLOR_LOC) out vec4 outColor;

void main() {
#ifndef FX_SKINNED
  vec4 inWeights = vec4(0.0), inBoneIds = vec4(0.0);
#endif
  vec3 worldPos, worldNormal;
  prop_skin(inPosition, inNormal, inWeights, inBoneIds, worldPos, worldNormal);
  gl_Position = prop_project(worldPos);

#ifdef FX_SCROLL
  float t = vc.Time.x * 0.01;
  outUv0 = vec4(inUv + vc.UVScroll0.xy * t, 0.0, 0.0);
#if FP_UVS >= 2
  outUv1 = vec4(inUv + vc.UVScroll0.zw * t, 0.0, 0.0);
#endif
#if FP_UVS >= 3
  outUv2 = vec4(inUv + vc.UVScroll1.xy * t, 0.0, 0.0);
#endif
#if FP_UVS >= 4
  outUv3 = vec4(inUv + vc.UVScroll1.zw * t, 0.0, 0.0);
#endif
#else
  outUv0 = vec4(inUv, 0.0, 0.0);
#endif
#ifdef FX_NOISE
  outNoiseUv = vec4(worldPos.xz * vc.NoiseParam.xy, 0.0, 0.0);
#endif
#ifdef FX_SPEC
  outNormal = vec4(-worldNormal, 0.0);
  outView = vec4(vc.EyePos.xyz - worldPos, 0.0);
#endif
#ifdef FX_LIGHTMAP
  outLightMapUv = vec4(worldPos.xz * vc.LightMapParam.xy * 0.5 + 0.5, 0.0, 0.0);
#endif
  outColor = prop_light(worldNormal, inColor);
}
#endif
#endif

#ifdef FX_PROP_FRAG
// ======================================================================= fragment
#if defined(FP_LOD) || defined(FX_SPEC) || defined(FX_LIGHTMAP) || defined(FX_NOISE) || defined(FX_REFLECT) || defined(FX_CC_MASK)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef FP_LOD
  vec4 MipmapBias;        // x: mip bias of the diffuse fetch
#endif
#ifdef FX_REFLECT
  vec4 ReflectAlpha;      // x: how much of the surface is mirror at full strength
#endif
#ifdef FX_CC_MASK
  vec4 ChangeColor[4];    // team / variant colours that replace the diffuse where the mask paints them
#endif
#ifdef FX_SPEC
  vec4 DifLghVec;         // the first light, for the half vector
  vec4 SpecularCol;
  vec4 SpecularLev;       // x
  vec4 SpecularPow;       // x exponent
#endif
#ifdef FX_LIGHTMAP
  vec4 LightMapParam;     // z: intensity
#endif
#ifdef FX_NOISE
  vec4 Noise;             // z: strength
#endif
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(set = 1, binding = 0) uniform samplerCube cubes[16];

layout(location = 0) in vec4 inUv0;
#if FP_UVS >= 2
layout(location = 1) in vec4 inUv1;
#endif
#if FP_UVS >= 3
layout(location = 2) in vec4 inUv2;
#endif
#if FP_UVS >= 4
layout(location = 3) in vec4 inUv3;
#endif
#ifdef FX_NOISE
layout(location = FP_NOISE_LOC) in vec4 inNoiseUv;
#endif
#ifdef FX_SPEC
layout(location = FP_NORMAL_LOC) in vec4 inNormal;
layout(location = FP_VIEW_LOC) in vec4 inView;
#endif
#ifdef FX_LIGHTMAP
layout(location = FP_LM_LOC) in vec4 inLightMapUv;
#endif
layout(location = FP_COLOR_LOC) in vec4 inColor;
layout(location = 0) out vec4 outColor;

// The diffuse layers, blended front over back (layer 0 at the back) and un-premultiplied; nothing visible gives zero.
vec4 prop_diffuse() {
#ifdef FP_LOD
  vec4 base = fx_texture_bias(textures[0], inUv0.xy, fc.MipmapBias.x);
#else
  vec4 base = texture(textures[0], inUv0.xy);
#endif
#if FP_UVS >= 2
  vec4 front = texture(textures[1], inUv1.xy);
  vec3 premult = base.rgb * base.a * (1.0 - front.a) + front.rgb * front.a;
  float alpha = base.a * (1.0 - front.a) + front.a;
#if FP_UVS >= 3
  front = texture(textures[2], inUv2.xy);
  premult = premult * (1.0 - front.a) + front.rgb * front.a;
  alpha = alpha * (1.0 - front.a) + front.a;
#endif
#if FP_UVS >= 4
  front = texture(textures[3], inUv3.xy);
  premult = premult * (1.0 - front.a) + front.rgb * front.a;
  alpha = alpha * (1.0 - front.a) + front.a;
#endif
  return alpha != 0.0 ? vec4(premult / alpha, alpha) : vec4(0.0);
#else
  return base;
#endif
}

void main() {
  vec4 diffuse = prop_diffuse();
#ifdef FX_CC_MASK
  // A four-channel mask paints up to four replacement colours over the diffuse; the mask value scales the colour.
  vec4 paint = texture(textures[12], inUv0.xy);
  if (paint.x != 0.0) diffuse.rgb = paint.x * fc.ChangeColor[0].xyz;
  else if (paint.y != 0.0) diffuse.rgb = paint.y * fc.ChangeColor[1].xyz;
  else if (paint.z != 0.0) diffuse.rgb = paint.z * fc.ChangeColor[2].xyz;
  else if (paint.w != 0.0) diffuse.rgb = paint.w * fc.ChangeColor[3].xyz;
#endif
#ifdef FX_LOGO
  // The logo (an emblem texture) is laid over the diffuse by its own alpha.
  vec4 logo = texture(textures[14], inUv0.xy);
  diffuse = vec4(diffuse.rgb * (1.0 - logo.a) + logo.rgb * logo.a, max(logo.a, diffuse.a));
#endif
  vec3 rgb = diffuse.rgb * inColor.rgb;
#ifdef FX_NOISE
  rgb += (texture(textures[6], inNoiseUv.xy).rgb * 2.0 - 1.0) * fc.Noise.z;
  rgb = max(rgb, vec3(0.0));
#endif
#ifdef FX_SPEC
  // Blinn highlight of the first light.
  vec3 n = fx_normalize(inNormal.xyz);
  vec3 half_ = fx_normalize(fx_normalize(inView.xyz) - fc.DifLghVec.xyz);
  float highlight = fx_pow(max(dot(n, half_), 0.0), fc.SpecularPow.x);
  vec3 specular = fc.SpecularCol.xyz * fc.SpecularLev.x;
#ifdef FX_SPEC_MAP
  specular *= texture(textures[5], inUv0.xy).w;
#endif
#ifdef FX_ALPHA_GLOSS
  specular *= diffuse.a;          // the diffuse alpha is a gloss mask here, not transparency
#endif
#ifdef FX_REFLECT
  // Mirror of the room, as in the normal-mapped props below but with the vertex normal.
  vec3 viewDir = fx_normalize(inView.xyz);
  vec3 env = texture(cubes[8], reflect(-viewDir, n)).rgb;
  float edge = 1.0 - max(dot(n, viewDir), 0.0);
  float mirror = fc.ReflectAlpha.x * (0.2 + 0.8 * edge * edge * edge * edge);
#ifdef FX_REFLECT_MAP
  mirror *= texture(textures[9], inUv0.xy).x;
#endif
#ifdef FX_REFLECT_ADD
  rgb += env * mirror;            // some shaders add the mirror on top and leave the diffuse at full strength
#else
  rgb = rgb * (1.0 - mirror) + env * mirror;
#endif
#ifdef FX_LIGHTMAP
  rgb *= texture(textures[13], inLightMapUv.xy).xyz * fc.LightMapParam.z;   // reflective shaders: the light map shades the diffuse and mirror, not the highlight
#endif
#endif
  rgb += specular * highlight;
#endif
#if defined(FX_LIGHTMAP) && !defined(FX_REFLECT)
  rgb *= texture(textures[13], inLightMapUv.xy).xyz * fc.LightMapParam.z;
#endif
#ifdef FX_ALPHA_GLOSS
  outColor = vec4(rgb, inColor.a);
#else
  outColor = vec4(rgb, diffuse.a * inColor.a);
#endif
}
#endif

#ifdef FX_PSM_FRAG
// ======================================================================= fragment, shadow-receiving props
// Diffuse times vertex light plus the noise texture, then either (FX_SCREEN_SHADOW) darkened by a screen-space shadow texture,
// or (FX_LIGHT_SHADOW) darkened by a soft depth compare against a shadow map seen from the shadow light.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Noise;             // z: strength
#ifdef FX_LIGHTMAP
  vec4 LightMapParam;     // z: intensity
#endif
#ifdef FX_LIGHT_SHADOW
  vec4 ShadowPrm[3];      // [0] x,y: depth range of the light, z: bias, w: strength; [1] z: first falloff scale, w: its exponent, x: weight of the face-away fade
                          // [2] x: second offset, y: second scale, z: its exponent, w: blend between the two terms
#endif
} fc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) in vec4 inUv0;
layout(location = 1) in vec4 inNoiseUv;
#ifdef FX_LIGHT_SHADOW
layout(location = 3) in vec4 inLightProj;
layout(location = 4) in vec4 inLightFade;
#else
layout(location = 2) in vec4 inScreenProj;
#endif
#ifdef FX_LIGHTMAP
layout(location = 5) in vec4 inLightMapUv;
layout(location = 6) in vec4 inColor;
#else
layout(location = 5) in vec4 inColor;
#endif
layout(location = 0) out vec4 outColor;

void main() {
  vec4 diffuse = fx_texture_bias(textures[0], inUv0.xy, -2.2);
  vec3 noise = (texture(textures[6], inNoiseUv.xy).rgb * 2.0 - 1.0) * fc.Noise.z;
  vec3 lit = diffuse.rgb * inColor.rgb;
#ifdef FX_LIGHT_SHADOW
  // The shadow map holds depth in x and y and a second mask in z. The fragment's depth in the light, scaled to 0..1 over the light's range,
  // is compared with both depths; each difference becomes a soft 0..1 step, raised to its own power, and the two are blended.
  vec3 map = texture(textures[12], inLightProj.xy / inLightProj.w).xyz;
  float facing = 1.0 - inLightFade.z;                      // 1 where the surface looks at the light
  float depth = clamp((inLightProj.z - fc.ShadowPrm[0].x) / (fc.ShadowPrm[0].y - fc.ShadowPrm[0].x) - fc.ShadowPrm[0].z, 0.0, 1.0);
  float stepA = 1.0 - clamp((depth - map.x) * fc.ShadowPrm[1].z, 0.0, 1.0);
  float stepB = clamp(((depth - map.y) - fc.ShadowPrm[2].x) * fc.ShadowPrm[2].y, 0.0, 1.0);
  float termA = 1.0 - fx_pow(stepA, fc.ShadowPrm[1].w);
  float termB = fx_pow(1.0 - stepB, fc.ShadowPrm[2].z);
  float shadowA = facing * fc.ShadowPrm[0].w * (termA * (1.0 + (termB - 1.0) * fc.ShadowPrm[2].w));
  float shadowB = facing * fc.ShadowPrm[1].x * (1.0 - map.z);
  vec3 rgb = lit * (1.0 - max(shadowA, shadowB)) + noise;
  rgb = max(rgb, vec3(0.0));
#else
  vec3 rgb = max(lit + noise, vec3(0.0));
#endif
#ifdef FX_SCREEN_SHADOW
  // The clip w is never zero on screen; the clamp and the zero test only keep an off-screen lookup finite (the game gives 0 for 0 / 0).
  vec2 shadowUv = vec2(inScreenProj.x == 0.0 ? 0.0 : inScreenProj.x / inScreenProj.z, inScreenProj.y == 0.0 ? 0.0 : inScreenProj.y / inScreenProj.z);
  shadowUv = clamp(shadowUv, vec2(-1.0e30), vec2(1.0e30));
  rgb *= 1.0 - texture(textures[12], shadowUv).x;
#endif
#ifdef FX_LIGHTMAP
  rgb *= texture(textures[13], inLightMapUv.xy).xyz * fc.LightMapParam.z;   // the baked light of the arena floor, after the shadow
#endif
  outColor = vec4(rgb, diffuse.a * inColor.a);
}
#endif
#ifdef FX_BUMP_FRAG
// ======================================================================= fragment, normal-mapped props
// Lit per pixel: the normal comes from the normal map (slot 4) laid on the surface frame the vertex stage passes.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
  vec4 fvMin;
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 MipmapBias;        // x: mip bias of the diffuse fetch
  vec4 SpecularCol;
  vec4 SpecularLev;       // x
  vec4 SpecularPow;       // x exponent
#ifdef FX_REFLECT
  vec4 ReflectAlpha;      // x: how much of the surface is mirror at full strength
#endif
#ifdef FX_CC_MASK
  vec4 ChangeColor[4];    // team / variant colours that replace the diffuse where the mask paints them
#endif
#ifdef FX_LIGHTMAP
  vec4 LightMapParam;     // z: intensity
#endif
} fc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(set = 1, binding = 0) uniform samplerCube cubes[16];

layout(location = 0) in vec4 inUv0;         // xy uv, z highlight strength
layout(location = 1) in vec4 inView;
layout(location = 2) in vec4 inNegNormal;   // negated world normal
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec4 inBitangent;
#ifdef FX_LIGHTMAP
layout(location = 5) in vec4 inLightMapUv;
layout(location = 6) in vec4 inColor;
#else
layout(location = 5) in vec4 inColor;
#endif
layout(location = 0) out vec4 outColor;

void main() {
  vec4 diffuse = fx_texture_bias(textures[0], inUv0.xy, fc.MipmapBias.x);
#ifdef FX_CC_MASK
  // A four-channel mask paints up to four replacement colours over the diffuse; the mask value scales the colour.
  vec4 paint = texture(textures[12], inUv0.xy);
  if (paint.x != 0.0) diffuse.rgb = paint.x * fc.ChangeColor[0].xyz;
  else if (paint.y != 0.0) diffuse.rgb = paint.y * fc.ChangeColor[1].xyz;
  else if (paint.z != 0.0) diffuse.rgb = paint.z * fc.ChangeColor[2].xyz;
  else if (paint.w != 0.0) diffuse.rgb = paint.w * fc.ChangeColor[3].xyz;
#endif
#ifdef FX_LOGO
  vec4 logo = texture(textures[14], inUv0.xy);
  diffuse = vec4(diffuse.rgb * (1.0 - logo.a) + logo.rgb * logo.a, max(logo.a, diffuse.a));
#endif
  // The shading normal, in the same negated convention as the vertex normal.
  vec4 normalTex = texture(textures[4], inUv0.xy);
  vec3 map = normalTex.xyz * 2.0 - 1.0;
  vec3 negN = fx_normalize(map.x * inTangent.xyz - map.y * inBitangent.xyz + map.z * inNegNormal.xyz);
  vec3 n = -negN;

  vec4 lam = max(vec4(dot(n, fc.DifLghVec[0].xyz), dot(n, fc.DifLghVec[1].xyz), dot(n, fc.DifLghVec[2].xyz), dot(n, fc.DifLghVec[3].xyz)), fc.fvMin);
  vec3 lights = lam.x * fc.DifLghCol[0].xyz + lam.y * fc.DifLghCol[1].xyz + lam.z * fc.DifLghCol[2].xyz + lam.w * fc.DifLghCol[3].xyz;
  float skyT = 0.5 - dot(n, fc.SkyDir.xyz) * 0.5;
  vec3 lit = lights * fc.MatDifCol.xyz + mix(fc.SkyCol.xyz, fc.GroundCol.xyz, skyT) * fc.MatAmbCol.xyz;

  vec3 half_ = fx_normalize(fx_normalize(inView.xyz) - fc.DifLghVec[0].xyz);
  float highlight = fx_pow(max(dot(negN, half_), 0.0), fc.SpecularPow.x);
  vec3 specular = fc.SpecularCol.xyz * fc.SpecularLev.x;
#ifdef FX_SPEC_MAP
  specular *= texture(textures[5], inUv0.xy).w;
#endif
#ifdef FX_ALPHA_GLOSS
  specular *= diffuse.a;          // no specular map: the diffuse alpha is the gloss mask and the output alpha is the vertex alpha only
#endif
#ifdef FX_NORMAL_GLOSS
  specular *= normalTex.w;        // the normal map's alpha is the gloss mask; the diffuse alpha is not read and the output alpha is the vertex alpha only
#endif
  specular *= highlight * inUv0.z;

  vec3 rgb = diffuse.rgb * inColor.rgb * lit;
#ifdef FX_REFLECT
  // Mirror of the room: the eye ray reflected off the shading normal reads a cube map. A reflection-amount map (slot 9) times ReflectAlpha
  // gives the strength, which grows toward grazing angles (0.2 head-on up to 1.0 edge-on) and takes that share away from the lit diffuse.
  vec3 viewDir = fx_normalize(inView.xyz);
  vec3 env = texture(cubes[8], reflect(-viewDir, negN)).rgb;
  float edge = 1.0 - max(dot(negN, viewDir), 0.0);
  float mirror = fc.ReflectAlpha.x * (0.2 + 0.8 * edge * edge * edge * edge);
#ifdef FX_REFLECT_MAP
  mirror *= texture(textures[9], inUv0.xy).x;
#endif
#ifdef FX_REFLECT_ADD
  rgb += env * mirror;            // some shaders add the mirror on top and leave the diffuse at full strength
#else
  rgb = rgb * (1.0 - mirror) + env * mirror;
#endif
#endif
  rgb += specular;
#ifdef FX_LIGHTMAP
  rgb *= texture(textures[13], inLightMapUv.xy).xyz * fc.LightMapParam.z;
#endif
#if defined(FX_ALPHA_GLOSS) || defined(FX_NORMAL_GLOSS)
  outColor = vec4(rgb, inColor.a);
#else
  outColor = vec4(rgb, diffuse.a * inColor.a);
#endif
}
#endif
#endif
