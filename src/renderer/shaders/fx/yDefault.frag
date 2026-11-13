#version 450
// Default lit model, pixel stage. See docs\renderer\effects\yDefault.md.
// Options: FX_SHADOW (shadow-map darkening), FX_SPECULAR (one highlight from the first light), FX_SPEC_MAP (the highlight strength from a map's alpha),
//          FX_SPEC_TEX_ALPHA (the highlight strength from the diffuse texture's alpha, which then does not reach the output alpha).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#if defined(FX_SHADOW) || defined(FX_SPECULAR)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef FX_SHADOW
  vec4 ShadowPrm0;   // x, y depth range, z bias, w strength
  vec4 ShadowPrm1;   // x second-layer strength, y facing bias, z near slope, w near power
  vec4 ShadowPrm2;   // x far start, y far slope, z far power, w far weight
#endif
#ifdef FX_SPECULAR
  vec4 FirstLightVec;
  vec4 SpecularCol;
  vec4 SpecularLev;
  vec4 SpecularPow;
#endif
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) in vec4 inUv;
layout(location = 1) in vec4 inNormal;
layout(location = 2) in vec4 inView;
layout(location = 3) in vec4 inShadow;
layout(location = 4) in vec4 inLightDir;
layout(location = 5) in vec4 inDepth;
layout(location = 6) in vec4 inLit;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 diffuse = texture(textures[0], inUv.xy);
  vec3 color = diffuse.xyz * inLit.xyz;
  float alpha = inLit.w;
#ifndef FX_SPEC_TEX_ALPHA
  alpha *= diffuse.w;
#endif

#ifdef FX_SPECULAR
  // Blinn highlight against the first light.
  vec3 n = fx_normalize(inNormal.xyz);
  vec3 half_ = fx_normalize(fx_normalize(inView.xyz) - fc.FirstLightVec.xyz);
  float highlight = fx_pow(max(dot(n, half_), 0.0), fc.SpecularPow.x);
#ifdef FX_SPEC_MAP
  float level = fc.SpecularLev.x * texture(textures[1], inUv.xy).w;
#elif defined(FX_SPEC_TEX_ALPHA)
  float level = fc.SpecularLev.x * diffuse.w;
#else
  float level = fc.SpecularLev.x;
#endif
  color += fc.SpecularCol.xyz * level * highlight;
#endif

#ifdef FX_SHADOW
  vec4 map = textureLod(textures[12], inShadow.xy / (inShadow.w * inShadow.w), 0.0);
  float receiver = clamp(inDepth.x / inDepth.y - fc.ShadowPrm0.z, 0.0, 1.0);
  vec4 gap = vec4(receiver) - map;
  float facing = 1.0 - (1.0 - max(dot(inNormal.xyz, inLightDir.xyz), 0.0)) * fc.ShadowPrm1.y;
  float near = 1.0 - fx_pow(1.0 - clamp(gap.x * fc.ShadowPrm1.z, 0.0, 1.0), fc.ShadowPrm1.w);
  float far = fx_pow(1.0 - clamp((gap.y - fc.ShadowPrm2.x) * fc.ShadowPrm2.y, 0.0, 1.0), fc.ShadowPrm2.z);
  float layerA = near + (near * far - near) * fc.ShadowPrm2.w;
  float layerB = (1.0 - map.z) * fc.ShadowPrm1.x;
  color *= 1.0 - max(fc.ShadowPrm0.w * facing * layerA, facing * layerB);
#endif
  outColor = vec4(color, alpha);
}
