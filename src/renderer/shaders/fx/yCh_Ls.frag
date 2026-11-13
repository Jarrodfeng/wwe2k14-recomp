#version 450
// Character (wrestler) skin and cloth, pixel stage. See docs\renderer\effects\yCh_Ls.md.
// Options: FX_CUBE_SPECULAR (specular from the environment cube, otherwise from the four lights), FX_SHADOW (shadow-map darkening),
//          FX_RIM (screen-space rim map and the material debug controls).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
#include "fx_character.glsl"

layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 NoiseTile;        // x tiling, y roughening strength, w colour modulation
  vec4 NormalFlip;
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 Reflect;          // xyz colour, w exponent of the ground reflection
  vec4 LineStoneParam;   // xy uv scale, z gamma, w gain
  vec4 SpecCubPrm1;      // two specular lobes: x, y strength and power of one; z, w of the other
  vec4 SpecCubPrm3;
  vec4 FogColor;
#ifdef FX_CUBE_SPECULAR
  vec4 DifLghMax;
  vec4 View[3];
#else
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
#endif
#ifdef FX_SHADOW
  vec4 ShadowPrm[3];     // see ch_shadow_amount
  vec4 ViewLightDepth;   // the light's direction: row 2 of ViewLight
#endif
#ifdef FX_RIM
  vec4 RimLightParam;
  vec4 RimLightDepth;
  vec4 DebugPS0;         // y normal strength, z gloss scale
  vec4 DebugPS1;         // z blend towards the rim term, w blend towards white
#endif
} fc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];
#ifdef FX_CUBE_SPECULAR
layout(set = 1, binding = 0) uniform samplerCube cubes[16];
#endif

layout(location = 0) in vec4 inNormal;
layout(location = 1) in vec4 inUvFog;
layout(location = 2) in vec4 inView;
#ifdef FX_SHADOW
layout(location = 3) in vec4 inShadow;
#endif
#ifdef FX_RIM
layout(location = 5) in vec4 inRim;
#endif
layout(location = 0) out vec4 outColor;

// The two specular lobes, summed over the cube or the four lights.
float specularSum(vec3 reflected, float gloss) {
#ifdef FX_CUBE_SPECULAR
  vec3 viewDir = vec3(dot(reflected, fc.View[0].xyz), dot(reflected, fc.View[1].xyz), dot(reflected, fc.View[2].xyz));
  vec2 cube = texture(cubes[11], viewDir).xy * fc.DifLghMax.w;
  return gloss * (cube.x * fc.SpecCubPrm1.x + cube.y * fc.SpecCubPrm1.z);
#else
  float lobeA = 0.0, lobeB = 0.0;
  for (int i = 0; i < 4; ++i) {
    float aligned = max(dot(reflected, fc.DifLghVec[i].xyz), 0.0);
    lobeA += fx_pow(aligned, fc.SpecCubPrm1.y) * fc.DifLghCol[i].w;
    lobeB += fx_pow(aligned, fc.SpecCubPrm1.w) * fc.DifLghCol[i].w;
  }
  return gloss * (fc.SpecCubPrm1.x * lobeA + fc.SpecCubPrm1.z * lobeB);
#endif
}

void main() {
  vec2 uv = inUvFog.xy;
  float fog = inUvFog.z;
  vec3 N = inNormal.xyz;
  vec3 V = inView.xyz;

  float mask = texture(textures[6], uv).z;
  vec4 surface = texture(textures[1], uv);            // normal xy, gloss w
  vec4 noise = texture(textures[3], uv * fc.NoiseTile.x) * 2.0 - 1.0;

#ifdef FX_RIM
  float gloss = surface.w * fc.DebugPS0.z;
  vec2 normalMap = (surface.xy - 0.5) * fc.DebugPS0.y * -2.0;
#else
  float gloss = surface.w;
  vec2 normalMap = 1.0 - 2.0 * surface.xy;
#endif
  // The masked noise roughens the normal map.
  vec2 tangentOffset = normalMap + noise.xy * fc.NoiseTile.y * mask;

  vec3 n = fx_normalize(ch_bent_normal(N, tangentOffset, fc.NormalFlip.xyz));
  vec3 h = fx_normalize(noise.xyz + n);               // a second, noise-only normal for the look terms (the name in the game is unknown)
  float nv = dot(n, V);
  vec3 reflected = reflect(V, n);

  float specular = specularSum(reflected, gloss);

  // Hemisphere grey: the average of the sky-to-ground colour along the bent direction.
  float skyT = 0.5 * dot(h, fc.SkyDir.xyz) + 0.5;
  float hemisphere = dot(mix(fc.SkyCol.xyz, fc.GroundCol.xyz, skyT), vec3(1.0 / 3.0));

  // The tinted "line stone" lookup (stone sparkle) is reflected off h and shaped by a gamma curve.
  vec2 stoneUv = reflect(V, h).xy * fc.LineStoneParam.xy;
  vec3 tint = fx_pow(texture(textures[7], stoneUv).rgb * hemisphere * fc.LineStoneParam.w, fc.LineStoneParam.z);

  float groundReflection = max(-reflected.y * fx_pow(1.0 + nv, fc.Reflect.w), 0.0);

  vec3 base = tint * gloss + fc.SpecCubPrm3.xyz * specular + fc.Reflect.xyz * groundReflection * gloss;
  vec3 color = base * (1.0 + noise.w * fc.NoiseTile.w * mask);

#ifdef FX_SHADOW
  vec4 shadowMap = textureLod(textures[12], inShadow.xy / inShadow.w, 0.0);
  float lit = 1.0 - ch_shadow_amount(shadowMap, inShadow.z, fc.ShadowPrm[0], fc.ShadowPrm[1], fc.ShadowPrm[2], dot(n, fc.ViewLightDepth.xyz));
  color *= lit;
#endif

  color = mix(color, fc.FogColor.xyz, fog);

#ifdef FX_RIM
  // The rim map is read at the vertex-supplied screen position, faded by depth; the result stays apart from the shaded colour, which the debug controls blend with it.
  vec2 rimMap = texture(textures[15], inRim.xy).xy;
  float rim = fx_pow(max(rimMap.y - inRim.z, 0.0) * fc.RimLightDepth.z + rimMap.x, fc.RimLightParam.x);
  rim *= (1.0 - abs(nv)) * ((gloss - 1.0) * fc.RimLightParam.y + 1.0) * fc.RimLightParam.z;
  vec3 blended = mix(vec3(rim), color, fc.DebugPS1.z);
#ifdef FX_SHADOW
  float blendTarget = lit;        // with a shadow the debug fade goes to the lit factor, not to white
#else
  float blendTarget = 1.0;
#endif
  outColor = vec4(mix(vec3(blendTarget), blended, fc.DebugPS1.w), (gloss - 1.0) * fc.DebugPS1.z + 1.0);
#else
  outColor = vec4(color, gloss);
#endif
}
