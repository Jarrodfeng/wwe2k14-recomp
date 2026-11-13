// Shared by the wrestler gear and cloth effects (yCh_gear*, yCh_cloth*). Vertex stage: GEAR_VERTEX before including. See docs\renderer\effects\yCh_gear.md.
#ifndef FX_GEAR_GLSL
#define FX_GEAR_GLSL

#ifdef GEAR_VERTEX
// Options: FX_SKINNED (1-4 bones chosen by the bool constants, otherwise one rigid bone), FX_SHADOW (writes the shadow-map position),
//          FX_SCREEN_CONVERT (re-centres and rescales the clip position with ConvScreen).
// The cloth effects define FX_CLOTH. Rigid cloth adds the bone-moved position and normal to the unmoved ones (its matrices are offsets);
// skinned cloth uses position and normal as they come and skins only the tangent.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrixArray[63];
  vec4 EyePos;
  vec4 FogDistance;       // x start, y end, z 1 / (end - start)
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 NoiseLodParam;     // x, y: clip depth range over which the noise detail fades out
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
layout(location = 5) in vec3 inTangent;

layout(location = 0) out vec4 outNormal;
layout(location = 1) out vec4 outUvFog;
layout(location = 2) out vec4 outView;
layout(location = 3) out vec4 outNoiseLod;
layout(location = 4) out vec4 outShadow;
layout(location = 5) out vec4 outUnused;
layout(location = 6) out vec4 outRim;
layout(location = 7) out vec4 outTangent;
layout(location = 8) out vec4 outTint;
layout(location = 9) out vec4 outSpecular;

// 0 at from, 1 at to, clamped.
float ramp(float z, float from, float to) { return clamp((z - from) / (to - from), 0.0, 1.0); }

#ifdef FX_SKINNED
// The tangent follows at most the first three bones, even when the position follows four or five.
vec3 gear_skin_tangent(vec3 tangent, vec3 w, vec4 ids, int boneCount) {
  vec3 t0 = ch_bone_turn(ch_bone(ids.x), tangent);
  if (boneCount == 1) return t0;
  vec3 t1 = ch_bone_turn(ch_bone(ids.y), tangent);
  if (boneCount == 2) return mix(t1, t0, w.x);
  vec3 t2 = ch_bone_turn(ch_bone(ids.z), tangent);
  return t0 * w.x + t1 * w.y + t2 * (1.0 - w.x - w.y);
}
#endif

void main() {
  float flip = 1.0 + vc.NormalFlip.w;
  vec3 normalIn = inNormal * flip;
  vec3 tangentIn = inTangent * flip;
  vec3 position;
  vec3 normal;
  vec3 tangent;
#ifdef FX_SKINNED
  int boneCount = fx_bool(vc.Bools, 3) ? 4 : (fx_bool(vc.Bools, 2) ? 3 : (fx_bool(vc.Bools, 1) ? 2 : 1));
#ifdef FX_CLOTH
  // Skinned cloth arrives already in world space: only the tangent is skinned.
  position = inPosition.xyz;
  normal = normalIn;
#else
  ch_skin(inPosition, normalIn, inWeights, inBoneIds, boneCount, position, normal);
#endif
  tangent = gear_skin_tangent(tangentIn, inWeights, inBoneIds, boneCount);
#else
  int bone = ch_bone(inBoneId);
  position = ch_bone_point(bone, inPosition);
  normal = ch_bone_turn(bone, normalIn);
  tangent = ch_bone_turn(bone, tangentIn);
#endif

#if defined(FX_CLOTH) && !defined(FX_SKINNED)
  position += inPosition.xyz;
  normal += normalIn;
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
  outNoiseLod = vec4(ramp(clip.z, vc.NoiseLodParam.x, vc.NoiseLodParam.y));
#ifdef FX_SHADOW
  outShadow = vec4(dot(vc.TexProjLightXY[0], world), dot(vc.TexProjLightXY[1], world), dot(vc.ViewLightDepth, world), dot(vc.TexProjLightW, world));
#else
  outShadow = vec4(0.0);
#endif
  outUnused = vec4(0.0);
  // xy: where this vertex is on screen (0..1, y down), to read the screen-space rim map; z: rim depth fade; w: rim strength by distance.
  float rimStrength = mix(vc.RimLightParam3.z, vc.RimLightParam3.w, ramp(clip.w, vc.RimLightParam3.x, vc.RimLightParam3.y));
  outRim = vec4(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5, ramp(clip.w, vc.RimLightDepth.x, vc.RimLightDepth.y), rimStrength);
  outTangent = vec4(fx_normalize(tangent), 0.0);
  outTint = vec4(1.0, 1.0, 1.0, vc.MdlAlpha.x * vc.MatAmbCol.w * vc.MatDifCol.w);
  outSpecular = vec4(vc.KeyLightColor.xyz * keyHighlight * fresnel * clip.w * vc.Param0.x, 0.0);

#ifdef FX_SCREEN_CONVERT
  clip.xy = (ndc - vc.ConvScreen.xy) * vc.ConvScreen.w * clip.w;
#endif
  gl_Position = clip;
}
#endif // GEAR_VERTEX

#ifdef GEAR_PIXEL
#include "fx_character.glsl"
// Pixel stage. Options: FX_CUBE_SPECULAR (specular from the environment cube, otherwise from the four lights), FX_SHADOW (shadow-map darkening of the
// diffuse and specular light), FX_BLOOD (the blood / sweat / stain mask: albedo tint and less noise roughening),
// FX_MUSCLE (the flex effects' muscle normal map), FX_LOGO, FX_CHANGE_COLOR (team colour masks), FX_DEBUG (the material debug controls),
// FX_BLOOD_BUMP (where it is wet: a bumped normal from the mask's slope and an extra highlight).
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef FX_BLOOD
  vec4 Blood;            // x amount
  vec4 BSSBldClr;        // colour the blood / sweat / stain mask tints the albedo towards
#endif
  vec4 NoiseTile;        // near: x tiling, y roughening strength, z albedo modulation, w specular modulation
  vec4 NoiseTileF;       // far: the same, faded in with the noise ramp
  vec4 NormalFlip;
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 Reflect;          // xyz colour, w exponent of the ground reflection
  vec4 SpecCubPrm1;      // two specular lobes: x, y strength and power of one; z, w of the other
  vec4 SpecCubPrm3;
  vec4 FogColor;
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 Param0;           // x key-light specular gain, y Fresnel power
  vec4 RimLightParam;
  vec4 RimLightDepth;
  vec4 fvLightLimit;
  vec4 DifLghMax;
  vec4 KeyLightColor;
  vec4 KeyLightVec;
#ifdef FX_CUBE_SPECULAR
  vec4 View[3];
#else
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
#if defined(FX_BLOOD_BUMP)
  vec4 Sweat;            // y: power of the wet highlight
#endif
#endif
#ifdef FX_SHADOW
  vec4 ShadowPrm[3];     // see ch_shadow_amount
  vec4 ViewLightDepth;   // the light's direction: row 2 of ViewLight
#endif
#ifdef FX_LOGO
  uvec4 Bools[2];        // bit 150 picks which logo texture is read
#endif
#ifdef FX_CHANGE_COLOR
  vec4 ChangeColor[16];  // one flat colour per mask channel
#endif
#ifdef FX_MUSCLE
  vec4 MuscleParam;      // x scale of the normal map where muscle shows, y how much of the muscle normal
  vec4 MuscleUV;         // the part of the texture the muscle map covers: xy from, zw to
#endif
#ifdef FX_DEBUG
  vec4 DebugPS0;         // x texture contrast, y normal strength, z gloss scale
  vec4 DebugPS1;         // z blend towards the rim term, w blend towards white
#endif
} fc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(set = 1, binding = 0) uniform samplerCube cubes[16];

layout(location = 0) in vec4 inNormal;
layout(location = 1) in vec4 inUvFog;
layout(location = 2) in vec4 inView;
layout(location = 3) in vec4 inNoiseLod;
#ifdef FX_SHADOW
layout(location = 4) in vec4 inShadow;
#endif
layout(location = 6) in vec4 inRim;
layout(location = 8) in vec4 inTint;
#ifdef FX_DEBUG
layout(location = 9) in vec4 inSpecular;   // the vertex stage's own key-light highlight
#endif
layout(location = 0) out vec4 outColor;

#ifdef FX_CUBE_SPECULAR
// The environment cube is read in view space.
vec3 gear_view_dir(vec3 d) { return vec3(dot(d, fc.View[0].xyz), dot(d, fc.View[1].xyz), dot(d, fc.View[2].xyz)); }
#endif

// The two specular lobes, summed over the cube or the four lights.
float gear_specular(vec3 reflected, float gloss) {
#ifdef FX_CUBE_SPECULAR
  vec2 cube = texture(cubes[11], gear_view_dir(reflected)).xy * fc.DifLghMax.w;
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

#ifdef FX_CHANGE_COLOR
// Team colours: sixteen mask channels, in order. The first channel that is not exactly zero replaces the texture colour with its own colour, scaled by the mask.
vec3 gear_change_color(vec3 base, vec2 uv) {
  vec4 masks[4] = vec4[4](texture(textures[7], uv), texture(textures[4], uv), texture(textures[14], uv), texture(textures[2], uv));
  for (int i = 15; i >= 0; --i) {
    float mask = masks[i >> 2][i & 3];
    if (mask != 0.0) base = mask * fc.ChangeColor[i].rgb;
  }
  return base;
}
#endif

void main() {
  vec2 uv = inUvFog.xy;
  float fog = inUvFog.z;
  vec3 N = inNormal.xyz;
  vec3 V = inView.xyz;
  float ramp = inNoiseLod.x;

  vec3 diffuse = texture(textures[0], uv).rgb;
#ifdef FX_LOGO
  vec4 logo = fx_bool(fc.Bools, 150) ? texture(textures[7], uv) : texture(textures[4], uv);
  diffuse = mix(diffuse, logo.rgb, logo.a);
#endif
#ifdef FX_CHANGE_COLOR
#ifdef FX_DEBUG
  diffuse = (diffuse - 0.5) * fc.DebugPS0.x + 0.5;   // the debug contrast reaches only the untouched texture colour (no team-colour mask)
#endif
  diffuse = gear_change_color(diffuse, uv);
#endif
  vec4 surface = texture(textures[1], uv);
#ifdef FX_TRANS
  // See-through cloth: the surface map's blue is the gloss and its alpha is the cloth's opacity.
  float opacity = surface.w;
  surface.w = surface.z;
#else
  float opacity = 1.0;
#endif
  float mask = texture(textures[6], uv).z;
  vec4 noiseTex = texture(textures[3], uv * fc.NoiseTile.x);

#ifdef FX_DEBUG
  float gloss = surface.w * fc.DebugPS0.z;
  vec2 normalMap = (surface.xy - 0.5) * fc.DebugPS0.y * -2.0;
#else
  float gloss = surface.w;
  vec2 normalMap = 1.0 - 2.0 * surface.xy;
#endif
#ifdef FX_MUSCLE
  // The flex effects: a flexing muscle has its own normal map (texture 2), faded in by its alpha.
  vec4 muscle = texture(textures[2], (uv - fc.MuscleUV.xy) * (1.0 / (fc.MuscleUV.zw - fc.MuscleUV.xy)));
  normalMap = mix(normalMap * mix(1.0, fc.MuscleParam.x, muscle.w), 1.0 - 2.0 * muscle.xy, fc.MuscleParam.y * muscle.w);
#endif
  vec2 noiseXY = noiseTex.xy * 2.0 - 1.0;
  float noiseW = noiseTex.w * 2.0 - 1.0;
  float roughen = mix(fc.NoiseTile.y, fc.NoiseTileF.y, ramp);
  float albedoMod = mix(fc.NoiseTile.z, fc.NoiseTileF.z, ramp);
  float specMod = mix(fc.NoiseTile.w, fc.NoiseTileF.w, ramp);

#ifdef FX_BLOOD
  float bss = texture(textures[14], uv).x;
  float bloodRaw = fc.Blood.x * bss * bss;
  float blood = min(bloodRaw, 1.0);
  vec3 albedo = diffuse * mix(vec3(1.0), fc.BSSBldClr.rgb, bss);
#else
  float blood = 0.0;
  vec3 albedo = diffuse;
#endif
  albedo *= 1.0 + noiseW * albedoMod * mask;

  vec2 offset = normalMap + noiseXY * roughen * mask * (1.0 - blood);
  vec3 n = fx_normalize(ch_bent_normal(N, offset, fc.NormalFlip.xyz));
  float nv = dot(n, V);
  vec3 reflected = reflect(V, n);

  float specular = gear_specular(reflected, gloss);
#ifdef FX_BLOOD_BUMP
  if (bloodRaw > 0.0) {
    // Where the mask is wet, the normal gets a bump from the mask's own slope (four taps) and the cube adds a sharper highlight.
    vec2 tap = vec2(0.000488281, 0.000976562);
    float d0 = bss - texture(textures[14], uv + vec2(tap.x, tap.y)).x;
    float d1 = bss - texture(textures[14], uv + vec2(tap.x, -tap.y)).x;
    float d2 = bss - texture(textures[14], uv + vec2(-tap.x, -tap.y)).x;
    float d3 = bss - texture(textures[14], uv + vec2(-tap.x, tap.y)).x;
    vec2 slope = vec2(d0 + d1 - d2 - d3, d0 - d1 - d2 + d3);
    vec3 nb = fx_normalize(ch_bent_normal(N, offset - bss * slope, fc.NormalFlip.xyz));
    vec3 wetReflected = reflect(V, nb);
#ifdef FX_CUBE_SPECULAR
    specular += texture(cubes[11], gear_view_dir(wetReflected)).z * fc.DifLghMax.w * bloodRaw;
#else
    float wetLobe = 0.0;
    for (int i = 0; i < 4; ++i) wetLobe += fx_pow(max(dot(wetReflected, fc.DifLghVec[i].xyz), 0.0), fc.Sweat.y) * fc.DifLghCol[i].w;
    specular += bloodRaw * wetLobe;
#endif
  }
#endif

  vec3 diffuseLight = min(texture(cubes[13], n).rgb * fc.DifLghMax.xyz, fc.fvLightLimit.xyz);
#ifdef FX_SHADOW
  vec4 shadowMap = textureLod(textures[12], inShadow.xy / inShadow.w, 0.0);
  float shadowLit = 1.0 - ch_shadow_amount(shadowMap, inShadow.z, fc.ShadowPrm[0], fc.ShadowPrm[1], fc.ShadowPrm[2], dot(n, fc.ViewLightDepth.xyz));
  diffuseLight *= shadowLit;
#endif
  vec3 hemisphere = mix(fc.SkyCol.xyz, fc.GroundCol.xyz, 0.5 * -dot(n, fc.SkyDir.xyz) + 0.5);
  vec3 lit = albedo * inTint.rgb * fc.MatDifCol.xyz * (diffuseLight + hemisphere * fc.MatAmbCol.xyz);

  float fresnel = fx_pow(1.0 - abs(nv), fc.Param0.y);
  float blinn = max(dot(fx_normalize(n + V), fc.KeyLightVec.xyz), 0.0);
  vec3 keySpecular = fc.KeyLightColor.xyz * blinn * fresnel * fc.Param0.x;

  vec2 rimMap = texture(textures[15], inRim.xy).xy;
  float rim = fx_pow(max(rimMap.y - inRim.z, 0.0) * fc.RimLightDepth.z + rimMap.x, fc.RimLightParam.x);
  rim *= (1.0 - abs(nv)) * ((gloss - 1.0) * fc.RimLightParam.y + 1.0) * fc.RimLightParam.z;
  vec3 boosted = keySpecular * inRim.w;
#ifdef FX_DEBUG
  boosted = mix(inSpecular.xyz, boosted, fc.DebugPS1.y);   // y blends the vertex highlight towards the pixel one
  float rimShade = rim * fc.DebugPS1.x * fc.DebugPS1.x;   // x applies twice: to the highlight term and to the rim weight
#else
  float rimShade = rim;
#endif
  vec3 shaded = mix(lit, 0.75 * (lit + boosted) + 0.25 * lit * (1.0 + boosted), rimShade);

  float groundReflection = max(-reflected.y * fx_pow(1.0 + nv, fc.Reflect.w), 0.0);
  vec3 sheen = fc.SpecCubPrm3.xyz * specular + fc.Reflect.xyz * groundReflection * gloss;
  sheen *= 1.0 + noiseW * specMod * (1.0 - blood) * mask;
#ifdef FX_SHADOW
  sheen *= shadowLit;
#endif

  vec3 color = shaded * (1.0 - min(sheen, vec3(1.0))) + sheen;
#if defined(FX_DEBUG)
#ifdef FX_SHADOW
#define DEBUG_WHITE shadowLit   // the debug output blends towards the shadow term, not white, where there is a shadow map
#else
#define DEBUG_WHITE 1.0
#endif
#endif
  color = mix(color, fc.FogColor.xyz, fog);
#ifdef FX_DEBUG
  outColor = vec4(mix(vec3(DEBUG_WHITE), mix(vec3(rim), color, fc.DebugPS1.z), fc.DebugPS1.w), mix(1.0, inTint.w * opacity, fc.DebugPS1.z));
#else
  outColor = vec4(color, inTint.w * opacity);
#endif
}
#endif // GEAR_PIXEL

#endif
