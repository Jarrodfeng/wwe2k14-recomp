// Pixel stage shared by the transparent-capable character effects (yCh_trans, yCh_TransTiling, yCh_Reflect). See docs\renderer\effects\yCh_trans.md.
// The including .frag starts with #version 450 and the include-directive extension, includes fx_common.glsl, fx_character.glsl and fx_trans.glsl, then this file.
// Options: FX_CUBE_SPECULAR (specular from the environment cube, otherwise from four lights), FX_SHADOW (shadow-map darkening),
//          FX_ENV_LIGHT (soft light from a cube read with the view-space normal, through a mask), FX_LOGO (one of two logo textures over the diffuse),
//          FX_CAMO (colour-change camouflage from sixteen mask channels), FX_NOISE (noise tile roughens the normal and modulates colour and specular),
//          FX_REFLECT (gloss in the normal map's w, opaque, ambient scaled by Param1.z),
//          FX_HAIR (yCh_Anisotropic, yCh_Aniso_trans: tangent frame from the vertex, two shifted highlight lobes along the hair, gloss in the normal map's w),
//          FX_HAIR_TRANS (with FX_HAIR: gloss in the normal map's z and opacity in its w, as in yCh_trans, and no environment specular),
//          FX_SS (four lights with a subsurface glow instead of the light cube), FX_LOGO_BLEND (the logo is blended in by its alpha; the opacity stays the normal map's),
//          FX_DEBUG (material debug controls: scaled normal map and gloss, key light blended with the per-vertex one, output blended towards the rim term and white).

layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 DifLghMax;        // xyz scale of the light cube, w scale of the specular cube
  vec4 LightLimit;
  vec4 FogColor;
  vec4 Level;            // w: mip level of the diffuse texture
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 NormalFlip;
  vec4 Param0;           // x key highlight gain, y Fresnel power
  vec4 Reflect;          // xyz colour, w exponent of the ground reflection
  vec4 RimLightDepth;
  vec4 RimLightParam;
#ifndef FX_HAIR_TRANS
  vec4 SpecCubPrm1;
  vec4 SpecCubPrm3;
#endif
  vec4 KeyLightColor;    // DifLghCol[4]
  vec4 KeyLightVec;      // DifLghVec[4]
#ifdef FX_CUBE_SPECULAR
  vec4 View[3];
#endif
#if (!defined(FX_CUBE_SPECULAR) && !defined(FX_HAIR_TRANS)) || defined(FX_SS)
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
#endif
#ifdef FX_SS
  vec4 SS;               // x glow strength, y and z: the facing range the glow ramps over (z start, y end)
  vec4 SSColor;          // rgb colour of the glow
#endif
#ifdef FX_NOISE
  vec4 NoiseTile;        // x tiling, y normal roughening, z colour modulation, w specular modulation
#endif
#ifdef FX_REFLECT
  vec4 Param1;           // z: ambient scale
#endif
#ifdef FX_HAIR
  vec4 HairSpeParam;      // xy shift of the two lobes, zw their sharpness
  vec4 SpeCol1;          // rgb colour of the first lobe, w tiling of the hair textures in u
  vec4 SpeCol2;          // rgb colour of the second lobe, w tiling in v
#endif
#ifdef FX_DEBUG
  vec4 DebugPS0;         // y normal strength, z gloss scale
  vec4 DebugPS1;         // x rim gain, y key light mix with the per-vertex one, z blend towards the rim term, w blend towards white
#endif
#ifdef FX_CAMO
  vec4 ChangeColor[16];  // one flat colour per mask channel
#endif
#ifdef FX_LOGO
  uvec4 Bools[2];        // bit 150 picks which logo texture is read
#endif
#ifdef FX_SHADOW
  vec4 ShadowPrm[3];     // see ch_shadow_amount
  vec4 ViewLightDepth;   // the light's direction: row 2 of ViewLight
#endif
} fc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(set = 1, binding = 0) uniform samplerCube cubes[16];

layout(location = 0) in vec4 inNormal;
layout(location = 1) in vec4 inUvFog;
layout(location = 2) in vec4 inView;
#ifdef FX_SHADOW
layout(location = 3) in vec4 inShadow;
#endif
layout(location = 5) in vec4 inRim;
#ifdef FX_HAIR
layout(location = 6) in vec4 inTangent;
layout(location = 7) in vec4 inTint;
#else
layout(location = 6) in vec4 inTint;
#endif
#if defined(FX_DEBUG) && defined(FX_HAIR)
layout(location = 8) in vec4 inVertexKey;   // the key highlight computed per vertex (the hair tangent takes interpolator 6, so it moves up one)
#elif defined(FX_DEBUG)
layout(location = 7) in vec4 inVertexKey;   // the key highlight computed per vertex
#endif
#ifdef FX_ENV_LIGHT
layout(location = 8) in vec4 inViewNormal;
#endif
layout(location = 0) out vec4 outColor;

#ifndef FX_HAIR_TRANS
// The two specular lobes, summed over the environment cube or the four lights.
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
#endif

#ifdef FX_HAIR
// One highlight lobe: the hair direction shifted along the normal, against the half vector between the view and straight up; fades out as the direction turns away.
float hairLobe(vec3 hairDir, vec3 halfVec, float sharpness) {
  float d = dot(fx_normalize(hairDir), halfVec);
  return smoothstep(-1.0, 0.0, d) * fx_pow(sqrt(max(1.0 - d * d, 0.0)), sharpness);
}

// The two highlights along the hair (colours SpeCol1 / SpeCol2, the second modulated by the noise map), shifted by a texture so the strands look uneven.
vec3 hairHighlights(vec3 binormal, vec3 n, vec3 v, vec2 uv, float gloss) {
  vec2 hairUv = uv * vec2(fc.SpeCol1.w, fc.SpeCol2.w);
  float shift = texture(textures[9], hairUv).x;
  float noise = texture(textures[8], hairUv).x;
  vec3 vn = fx_normalize(v);
  vec3 halfVec = fx_normalize(vec3(vn.x, vn.y - 1.0, vn.z));
  float lobe1 = hairLobe(binormal + n * (shift + fc.HairSpeParam.x - 0.5), halfVec, fc.HairSpeParam.z);
  float lobe2 = hairLobe(binormal + n * (shift + fc.HairSpeParam.y - 0.5), halfVec, fc.HairSpeParam.w);
  return gloss * (fc.SpeCol1.xyz * lobe1 + fc.SpeCol2.xyz * (noise * lobe2));
}
#endif

#ifdef FX_CAMO
// Colour-change camouflage: sixteen mask channels (four textures of four) each own a flat colour; the first channel in order that is not zero replaces the diffuse colour.
vec3 camoAlbedo(vec2 uv, vec3 base) {
  vec4 m0 = texture(textures[7], uv);
  vec4 m1 = texture(textures[4], uv);
  vec4 m2 = texture(textures[14], uv);
  vec4 m3 = texture(textures[2], uv);
  float masks[16] = float[16](m0.x, m0.y, m0.z, m0.w, m1.x, m1.y, m1.z, m1.w, m2.x, m2.y, m2.z, m2.w, m3.x, m3.y, m3.z, m3.w);
  for (int i = 0; i < 16; ++i) {
    if (masks[i] != 0.0) return masks[i] * fc.ChangeColor[i].xyz;
  }
  return base;
}
#endif

void main() {
  vec2 uv = inUvFog.xy;
  float fog = inUvFog.z;
  vec3 V = inView.xyz;

  vec4 surface = texture(textures[1], uv);            // normal xy, gloss z (w with FX_REFLECT), opacity w
#if defined(FX_REFLECT) || (defined(FX_HAIR) && !defined(FX_HAIR_TRANS))
  float gloss = surface.w;
  float opacity = 1.0;
#else
  float gloss = surface.z;
  float opacity = surface.w;
#endif
#ifdef FX_DEBUG
  gloss *= fc.DebugPS0.z;
  vec2 tangentOffset = (surface.xy - 0.5) * fc.DebugPS0.y * -2.0;
#else
  vec2 tangentOffset = 1.0 - 2.0 * surface.xy;
#endif

#ifdef FX_NOISE
  // The tiled noise roughens the normal and, through the mask, modulates colour and specular.
  vec4 noise = texture(textures[3], uv * fc.NoiseTile.x) * 2.0 - 1.0;   // x, y offsets, w brightness (z unused)
  float noiseMask = texture(textures[6], uv).z;
  tangentOffset += noise.xy * fc.NoiseTile.y * noiseMask;
#endif

#ifdef FX_HAIR
  // The hair runs along the binormal; the tangent comes from the vertex.
  vec3 hairBinormal = cross(inNormal.xyz, inTangent.xyz);
  vec3 n = fx_normalize(inNormal.xyz * fc.NormalFlip.z + inTangent.xyz * (tangentOffset.x * fc.NormalFlip.x) + hairBinormal * (tangentOffset.y * fc.NormalFlip.y));
#else
  vec3 n = fx_normalize(ch_bent_normal(inNormal.xyz, tangentOffset, fc.NormalFlip.xyz));
#endif
  float nv = dot(n, V);
  vec3 reflected = reflect(V, n);

  vec4 diffuseTex = textureLod(textures[0], uv, fc.Level.w);
#ifdef FX_LOGO
  // A logo texture (one of two, by a game switch) covers the diffuse wherever its alpha is above zero, and brings its own opacity.
  vec4 logo = fx_bool(fc.Bools, 150) ? texture(textures[7], uv) : texture(textures[4], uv);
#if defined(FX_REFLECT) || defined(FX_LOGO_BLEND)
  diffuseTex.rgb = mix(diffuseTex.rgb, logo.rgb, logo.w);     // yCh_Reflect and yCh_TransTiling blend the logo in by its alpha; the opacity stays the normal map's
#else
  if (logo.w > 0.0) diffuseTex = logo;
  opacity = diffuseTex.w;
#endif
#endif
  vec3 base = diffuseTex.rgb;
#ifdef FX_CAMO
  base = camoAlbedo(uv, base);
#endif
#ifdef FX_NOISE
  base *= 1.0 + noise.w * fc.NoiseTile.z * noiseMask;
#endif
  vec3 albedo = base * fc.MatDifCol.xyz * inTint.xyz;

  float lit = 1.0;
#ifdef FX_SHADOW
  vec4 shadowMap = textureLod(textures[12], inShadow.xy / inShadow.w, 0.0);
  lit = 1.0 - ch_shadow_amount(shadowMap, inShadow.z, fc.ShadowPrm[0], fc.ShadowPrm[1], fc.ShadowPrm[2], dot(n, fc.ViewLightDepth.xyz));
#endif
#ifdef FX_SS
  // Four lights instead of the light cube, each with a subsurface glow that peaks where the surface turns away from it; the glow goes through the mask's y.
  vec3 direct = vec3(0.0), glow = vec3(0.0);
  for (int i = 0; i < 4; ++i) {
    float facing = max(dot(n, fc.DifLghVec[i].xyz), 0.0);
    float t = clamp((facing - fc.SS.z) / (fc.SS.y - fc.SS.z), 0.0, 1.0);
    float s = t * t * (3.0 - 2.0 * t);
    direct += facing * fc.DifLghCol[i].xyz;
    glow += s * s * (1.0 - s) * fc.DifLghCol[i].xyz;
  }
  vec3 lightCube = min(direct + glow * fc.SSColor.xyz * (fc.SS.x * texture(textures[6], uv).y), fc.LightLimit.xyz) * lit;
#else
  vec3 lightCube = min(texture(cubes[13], n).xyz * fc.DifLghMax.xyz, fc.LightLimit.xyz) * lit;
#endif
  vec3 ambient = fc.MatAmbCol.xyz;
#ifdef FX_REFLECT
  ambient *= fc.Param1.z;
#endif
  vec3 diffuse = albedo * (trans_hemisphere(n, fc.SkyDir.xyz, fc.SkyCol.xyz, fc.GroundCol.xyz) * ambient + lightCube);

  float groundReflection = max(-reflected.y * fx_pow(1.0 + nv, fc.Reflect.w), 0.0);
  vec3 specular = fc.Reflect.xyz * (groundReflection * gloss);
#ifndef FX_HAIR_TRANS
  specular += fc.SpecCubPrm3.xyz * specularSum(reflected, gloss);   // see-through hair has no environment specular, only its own two lobes
#endif
#ifdef FX_ENV_LIGHT
  // Soft light from a cube looked up with the view-space normal (y flipped), through a mask map, added to the specular (before the noise modulation).
  vec3 envLight = texture(cubes[10], vec3(inViewNormal.x, -inViewNormal.y, inViewNormal.z)).xyz;
  specular += envLight * texture(textures[6], uv).y;
#endif
#ifdef FX_NOISE
  specular *= 1.0 + noise.w * fc.NoiseTile.w * noiseMask;
#endif
#ifdef FX_HAIR
  specular += hairHighlights(hairBinormal, n, V, uv, gloss);
#endif
  specular *= lit;

  vec3 key = trans_key_highlight(n, V, nv, fc.KeyLightVec.xyz, fc.KeyLightColor.xyz, fc.Param0.x, fc.Param0.y, inRim.w);
  float rim = trans_rim_weight(texture(textures[15], inRim.xy).xy, inRim.z, fc.RimLightDepth, fc.RimLightParam, gloss, nv);

#ifdef FX_DEBUG
  // Debug controls: the key light fades between the per-pixel and per-vertex versions, the rim weight is scaled, and the shaded colour is blended towards the plain rim term and then towards white.
  vec3 color = trans_compose(diffuse, mix(inVertexKey.xyz, key, fc.DebugPS1.y), rim * fc.DebugPS1.x * fc.DebugPS1.x, specular);
  color = mix(color, fc.FogColor.xyz, fog);
  float alpha = opacity * inTint.w;
  color = mix(vec3(rim), color, fc.DebugPS1.z);
  outColor = vec4(mix(vec3(lit), color, fc.DebugPS1.w), (alpha - 1.0) * fc.DebugPS1.z + 1.0);
#else
  vec3 color = trans_compose(diffuse, key, rim, specular);
  outColor = vec4(mix(color, fc.FogColor.xyz, fog), opacity * inTint.w);
#endif
}
