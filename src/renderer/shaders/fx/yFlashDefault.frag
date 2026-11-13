#version 450
// Lit, textured surface that can be lit by the camera flash (pixel stage). See docs\renderer\effects\yFlashDefault.md.
// Options: FX_SHADOW (darkens by the character shadow map, read at the projected position).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#ifdef FX_SHADOW
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 ShadowPrm[3];     // [0] z: depth bias, w: strength; [1] x: floor darkness, y: facing bias, z: near slope, w: near power; [2] x: far start, y: far slope, z: far power, w: far weight
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];   // 0: diffuse, 12: shadow map

layout(location = 0) in vec4 inUv;
#ifdef FX_SHADOW
layout(location = 1) in vec4 inNormal;
layout(location = 3) in vec4 inShadow;
layout(location = 4) in vec4 inToLight;
layout(location = 5) in vec4 inLightDepth;
#endif
layout(location = 6) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 diffuse = texture(textures[0], inUv.xy);
#ifdef FX_SHADOW
  // The game divides by w twice here, so the map is read at xy / w^2.
  vec3 map = textureLod(textures[12], inShadow.xy / (inShadow.w * inShadow.w), 0.0).xyz;
  float receiver = clamp(inLightDepth.x / inLightDepth.y - fc.ShadowPrm[0].z, 0.0, 1.0);
  float facing = 1.0 - (1.0 - max(dot(inToLight.xyz, inNormal.xyz), 0.0)) * fc.ShadowPrm[1].y;
  vec2 gap = vec2(receiver) - map.xy;
  float nearPart = 1.0 - fx_pow(1.0 - clamp(gap.x * fc.ShadowPrm[1].z, 0.0, 1.0), fc.ShadowPrm[1].w);
  float farPart = fx_pow(1.0 - clamp((gap.y - fc.ShadowPrm[2].x) * fc.ShadowPrm[2].y, 0.0, 1.0), fc.ShadowPrm[2].z);
  float layer = nearPart + (nearPart * farPart - nearPart) * fc.ShadowPrm[2].w;
  float darkness = max(fc.ShadowPrm[0].w * facing * layer, facing * fc.ShadowPrm[1].x * (1.0 - map.z));
  outColor = vec4(diffuse.rgb * inColor.rgb * (1.0 - darkness), diffuse.a * inColor.a);
#else
  outColor = diffuse * inColor;
#endif
}
