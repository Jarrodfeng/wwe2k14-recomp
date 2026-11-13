#version 450
// Video playing on a surface (ring screen, tron), vertex stage. See docs\renderer\effects\yMovieDefault.md.
// Lit with the sky / ground hemisphere and four directional lights, colour handed on in the vertex colour output.
// Options: FX_SHADOW (also hands on the shadow-map position and the direction and depth toward the light).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrixArray[63];
  vec4 EyePos;
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

layout(location = 0) in vec4 inPosition;
layout(location = 1) in float inBoneId;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inUv;

layout(location = 0) out vec4 outUv;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outView;
layout(location = 3) out vec4 outShadow;
layout(location = 4) out vec4 outToLight;
layout(location = 5) out vec4 outLightDepth;
layout(location = 6) out vec4 outColor;

void main() {
  int b = fx_bool(vc.Bools, 0) ? 3 * int(inBoneId) : 0;
  vec3 world = vec3(dot(vc.WorldMatrixArray[b], inPosition), dot(vc.WorldMatrixArray[b + 1], inPosition), dot(vc.WorldMatrixArray[b + 2], inPosition));
  vec3 normal = vec3(dot(vc.WorldMatrixArray[b].xyz, inNormal), dot(vc.WorldMatrixArray[b + 1].xyz, inNormal), dot(vc.WorldMatrixArray[b + 2].xyz, inNormal));
  vec4 w4 = vec4(world, 1.0);
  gl_Position = vec4(dot(vc.ViewProj[0], w4), dot(vc.ViewProj[1], w4), dot(vc.ViewProj[2], w4), dot(vc.ViewProj[3], w4));

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
  outColor = vec4(diffuse * vc.MatDifCol.xyz + ambient, vc.MatAmbCol.w * vc.MatDifCol.w * vc.MdlAlpha.x) * inColor;
}
