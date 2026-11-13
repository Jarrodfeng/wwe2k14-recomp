#version 450
// Crowd members, pixel stage. See docs\renderer\effects\yAudience.md.
// Options: FX_NORMAL_MAP (lights per pixel from the normal map and the tangent frame), FX_SPECULAR (adds a glint from the first light; needs FX_NORMAL_MAP),
//          FX_LM_PIXEL (multiplies by the baked light map), FX_COLOR_ALPHA (the vertex alpha scales the result's alpha).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#if defined(FX_LM_PIXEL) || defined(FX_NORMAL_MAP)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef FX_LM_PIXEL
  vec4 LightMapParam;       // z brightness
#endif
#ifdef FX_NORMAL_MAP
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 LightMin;
#endif
#if defined(FX_NORMAL_MAP) && defined(FX_SPECULAR)
  vec4 SpecularCol;
  vec4 SpecularLev;         // x
  vec4 SpecularPow;         // x
#endif
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) in vec4 inUv;
layout(location = 1) in vec4 inShade;
#ifdef FX_NORMAL_MAP
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec4 inBitangent;
layout(location = 4) in vec4 inSlotA;
#ifdef FX_SPECULAR
layout(location = 5) in vec4 inSlotB;
#endif
#endif
layout(location = 0) out vec4 outColor;

#ifdef FX_NORMAL_MAP
vec3 audience_lighting(vec3 n) {
  vec3 diffuse = vec3(0.0);
  for (int i = 0; i < 4; ++i) diffuse += max(dot(n, -fc.DifLghVec[i].xyz), fc.LightMin[i]) * fc.DifLghCol[i].xyz;
  vec3 ambient = mix(fc.SkyCol.xyz, fc.GroundCol.xyz, 0.5 * dot(n, fc.SkyDir.xyz) + 0.5) * fc.MatAmbCol.xyz;
  return diffuse * fc.MatDifCol.xyz + ambient;
}
#endif

void main() {
  vec4 diffuse = texture(textures[0], inUv.xy);
#ifdef FX_LM_PIXEL
  vec3 pixelLightMap = texture(textures[13], inUv.zw).rgb * fc.LightMapParam.z;
#else
  vec3 pixelLightMap = vec3(1.0);
#endif

#ifdef FX_NORMAL_MAP
  vec4 normalSample = texture(textures[2], inUv.xy);     // xy slope, z glint mask
  vec2 slope = normalSample.xy * 2.0 - 1.0;
  float up = sqrt(abs(1.0 - dot(slope, slope)));
  vec3 n = fx_normalize(up * inShade.xyz + slope.x * inTangent.xyz - slope.y * inBitangent.xyz);
#if defined(FX_SPECULAR)
  vec3 vertexLightMap = inSlotB.rgb;
#else
  vec3 vertexLightMap = inSlotA.rgb;
#endif
  vec3 color = audience_lighting(n) * vertexLightMap * diffuse.rgb * pixelLightMap;
#ifdef FX_SPECULAR
  vec3 halfVec = fx_normalize(fx_normalize(inSlotA.xyz) - fc.DifLghVec[0].xyz);
  float glint = fx_pow(max(dot(n, halfVec), 0.0), fc.SpecularPow.x);
  color += fc.SpecularCol.xyz * fc.SpecularLev.x * glint * normalSample.z;
#endif
  outColor = vec4(color, diffuse.a);
#else
  float alpha = diffuse.a;
#ifdef FX_COLOR_ALPHA
  alpha *= inShade.a;
#endif
  outColor = vec4(diffuse.rgb * inShade.rgb * pixelLightMap, alpha);
#endif
}
