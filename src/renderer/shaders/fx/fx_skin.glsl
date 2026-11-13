// Shared by the wrestler skin effects (yCh_skin, yCh_skin_flex). See docs\renderer\effects\yCh_skin.md.
// Needs fx_common.glsl first. Holds the whole vertex shader (SKIN_VERTEX) and the whole fragment shader (SKIN_PIXEL); each effect's .vert and .frag only pick one.
#ifndef FX_SKIN_GLSL
#define FX_SKIN_GLSL

#ifdef SKIN_VERTEX
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
  vec4 KeyLightColor;     // the fifth light, the one this stage shades with
  vec4 KeyLightVec;
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
#ifdef FX_SPHERE_OCC
  vec4 SphereOccP[2];     // xyz centre, w radius
  vec4 SphereOccV[2];     // xyz axis, w strength
#endif
#ifdef FX_ID_MAP
  vec4 IdMapCol;          // the colour an ID-map slot tints towards
  vec4 IdMapLvLst[2];     // how far each of the eight slots tints
#endif
#ifdef FX_WRINKLE_LEVEL
  vec4 WklLvLst[2];       // how deep each of the six wrinkle slots is
#endif
} vc;

#define CH_BONES vc.WorldMatrixArray
#include "fx_character.glsl"
// Skins a point, its normal and its tangent. Unlike the Ls effect the normal follows every bone in use (up to four);
// only the tangent stops at three bones. boneCount (1..4) is picked from the bool constants by skin_bone_count; the
// tangent count comes from skin_tangent_count (the game checks one more flag there, which also means "three or more").
void skin_blend(vec4 position, vec3 normal, vec3 tangent, vec3 w, vec4 ids, int boneCount, int tangentCount,
                out vec3 worldPos, out vec3 worldNormal, out vec3 worldTangent) {
  int b0 = ch_bone(ids.x);
  vec3 p0 = ch_bone_point(b0, position);
  vec3 n0 = ch_bone_turn(b0, normal);
  vec3 t0 = ch_bone_turn(b0, tangent);
  worldPos = p0;
  worldNormal = n0;
  worldTangent = t0;
  if (boneCount == 1 && tangentCount == 1) return;
  int b1 = ch_bone(ids.y);
  vec3 p1 = ch_bone_point(b1, position);
  vec3 n1 = ch_bone_turn(b1, normal);
  vec3 t1 = ch_bone_turn(b1, tangent);
  if (boneCount == 2) { worldPos = mix(p1, p0, w.x); worldNormal = mix(n1, n0, w.x); }
  if (tangentCount == 2) worldTangent = mix(t1, t0, w.x);
  if (boneCount < 3 && tangentCount < 3) return;
  int b2 = ch_bone(ids.z);
  vec3 p2 = ch_bone_point(b2, position);
  vec3 n2 = ch_bone_turn(b2, normal);
  vec3 t2 = ch_bone_turn(b2, tangent);
  float rest = 1.0 - w.x - w.y;
  if (tangentCount >= 3) worldTangent = t0 * w.x + t1 * w.y + t2 * rest;
  if (boneCount >= 3) worldNormal = n0 * w.x + n1 * w.y + n2 * rest;
  if (boneCount == 3) worldPos = p0 * w.x + p1 * w.y + p2 * rest;
  if (boneCount == 4) worldPos = p0 * w.x + p1 * w.y + p2 * w.z + ch_bone_point(ch_bone(ids.w), position) * (rest - w.z);
}
int skin_bone_count(uvec4 bools[2]) { return fx_bool(bools, 3) ? 4 : (fx_bool(bools, 2) ? 3 : (fx_bool(bools, 1) ? 2 : 1)); }
int skin_tangent_count(uvec4 bools[2]) { return (fx_bool(bools, 4) || fx_bool(bools, 3) || fx_bool(bools, 2)) ? 3 : (fx_bool(bools, 1) ? 2 : 1); }

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
layout(location = 6) in vec4 inExtra;      // x: a per-vertex scalar passed on (location 9), w: goes to the fourth component of location 1

layout(location = 0) out vec4 outNormal;
layout(location = 1) out vec4 outUvFog;
layout(location = 2) out vec4 outView;
layout(location = 3) out vec4 outShadow;
layout(location = 4) out vec4 outOcclusion;
layout(location = 5) out vec4 outRim;
layout(location = 6) out vec4 outTangent;
layout(location = 7) out vec4 outTint;
layout(location = 8) out vec4 outSpecular;
layout(location = 9) out vec4 outExtra;

float ramp(float z, float from, float to) { return clamp((z - from) / (to - from), 0.0, 1.0); }

#ifdef FX_SPHERE_OCC
// One sphere: strength x how directly the vertex lies along the sphere's axis x how deep inside its radius it is.
float sphere_occlusion(vec3 position, vec4 centre, vec4 axis) {
  vec3 away = position - centre.xyz;
  float dist = length(away);
  float along = clamp(dot(fx_normalize(axis.xyz), fx_normalize(away)), 0.0, 1.0);
  return axis.w * along * (1.0 - min(dist / centre.w, 1.0));
}
#endif

void main() {
  vec3 normalIn = inNormal * (1.0 + vc.NormalFlip.w);
  vec3 tangentIn = inTangent * (1.0 + vc.NormalFlip.w);
  vec3 position, normal, tangent;
#ifdef FX_SKINNED
  skin_blend(inPosition, normalIn, tangentIn, inWeights, inBoneIds, skin_bone_count(vc.Bools), skin_tangent_count(vc.Bools), position, normal, tangent);
#else
  int bone = ch_bone(inBoneId);
  position = ch_bone_point(bone, inPosition);
  normal = ch_bone_turn(bone, normalIn);
  tangent = ch_bone_turn(bone, tangentIn);
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
  // inExtra.w can be a packed byte: low three bits the ID-map slot, the rest the wrinkle slot.
  float wrinkleLevel = inExtra.w;
  vec3 tint = vec3(1.0);
#if defined(FX_ID_MAP) || defined(FX_WRINKLE_LEVEL)
  int packedSlots = int(inExtra.w * 255.5);
#endif
#ifdef FX_ID_MAP
  int idSlot = packedSlots & 7;
  tint = mix(vec3(1.0), vc.IdMapCol.xyz, vc.IdMapLvLst[idSlot >> 2][idSlot & 3]);
#endif
#ifdef FX_WRINKLE_LEVEL
  int wrinkleSlot = packedSlots >> 3;
  if (wrinkleSlot > 5) wrinkleSlot = 1;   // past the table the game falls back to the second entry
  wrinkleLevel = vc.WklLvLst[wrinkleSlot >> 2][wrinkleSlot & 3];
#endif

  outUvFog = vec4(inUv, (fogDepth - vc.FogDistance.x) * vc.FogDistance.z, wrinkleLevel);
  outView = vec4(v, 1.0);
#ifdef FX_SHADOW
  outShadow = vec4(dot(vc.TexProjLightXY[0], world), dot(vc.TexProjLightXY[1], world), dot(vc.ViewLightDepth, world), dot(vc.TexProjLightW, world));
#else
  outShadow = vec4(0.0);
#endif
#ifdef FX_SPHERE_OCC
  outOcclusion = vec4(sphere_occlusion(position, vc.SphereOccP[0], vc.SphereOccV[0]) + sphere_occlusion(position, vc.SphereOccP[1], vc.SphereOccV[1]), 0.0, 0.0, 0.0);
#else
  outOcclusion = vec4(0.0);
#endif
  float rimStrength = mix(vc.RimLightParam3.z, vc.RimLightParam3.w, ramp(clip.w, vc.RimLightParam3.x, vc.RimLightParam3.y));
  outRim = vec4(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5, ramp(clip.w, vc.RimLightDepth.x, vc.RimLightDepth.y), rimStrength);
  outTangent = vec4(fx_normalize(tangent), 0.0);
  outTint = vec4(tint, vc.MdlAlpha.x * vc.MatAmbCol.w * vc.MatDifCol.w);
  outSpecular = vec4(vc.KeyLightColor.xyz * keyHighlight * fresnel * clip.w * vc.Param0.x, 0.0);
  outExtra = vec4(inExtra.x, inExtra.x, inExtra.x, 0.0);

#ifdef FX_SCREEN_CONVERT
  clip.xy = (ndc - vc.ConvScreen.xy) * vc.ConvScreen.w * clip.w;
#endif
  gl_Position = clip;
}
#endif

#ifdef SKIN_PIXEL
#include "fx_character.glsl"

// Option sets the game never builds. Rejecting them here keeps the solver from trying them.
#if (defined(FX_LOGO) && (defined(FX_CHANGE_COLOR) || defined(FX_BSS))) || (defined(FX_CHANGE_COLOR) && defined(FX_BSS))
#error not a real option set
#endif
#if (defined(FX_SWEAT) && !defined(FX_BSS)) || (defined(FX_BLOOD) && !defined(FX_SWEAT)) || (defined(FX_BLOOD_TINT) && !defined(FX_BSS)) || (defined(FX_DEBUG) && defined(FX_BSS))
#error not a real option set
#endif

layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Level;             // w: the fixed mip level of the diffuse map
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 NormalFlip;
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 DifLghCol[5];      // [4] is the key light
  vec4 DifLghVec[5];
  vec4 DifLghMax;         // xyz diffuse cube gain, w specular cube gain
  vec4 DifLghMaxA;        // x how much of the cube's alpha feeds the sub-surface term
  vec4 LightLimit;
  vec4 SSS;               // xyz the lit sub-surface colour, w its strength
  vec4 SSSSinCol2;        // the sub-surface colour where no light arrives
  vec4 HdLig;             // x blow-out strength, y blow-out threshold
  vec4 Param0;            // x key highlight gain, y Fresnel power
  vec4 Reflect;           // xyz colour, w exponent of the ground reflection
  vec4 RimLightDepth;
  vec4 RimLightParam;
  vec4 SpecCubPrm1;       // two lobes: x, y strength and power of one; z, w of the other
  vec4 SpecCubPrm3;
  vec4 FogColor;
#ifdef FX_CHANGE_COLOR
  vec4 ChangeColor[16];
#endif
#ifdef FX_CUBE_SPECULAR
  vec4 View[3];
#endif
#ifdef FX_SHADOW
  vec4 ShadowPrm[3];      // see ch_shadow_amount
  vec4 ViewLightDepth;    // the light's direction: row 2 of ViewLight
#endif
#ifdef FX_WRINKLES
  vec4 WrinkUV;           // the part of the texture the wrinkle map covers: xy from, zw to
#endif
#ifdef FX_MUSCLE
  vec4 MuscleParam;       // x scale of the normal map where muscle shows, y how much of the muscle normal
  vec4 MuscleUV;          // the part of the texture the muscle map covers: xy from, zw to
#endif
#ifdef FX_BLOOD_TINT
  vec4 BSSBldClr;         // what blood multiplies the skin colour by
#endif
#ifdef FX_SWEAT
  vec4 Sweat;             // x amount, y power of the sharp highlight, z tiling of the bump map
#endif
#ifdef FX_BLOOD
  vec4 Blood;             // x how wet blood looks
#endif
#ifdef FX_LOGO
  uvec4 Bools[2];         // 150: the logo is in slot 7 instead of 4
#endif
#ifdef FX_DEBUG
  vec4 DebugPS0;          // x unused by the game's code, y normal strength, z gloss scale
  vec4 DebugPS1;          // x, y rim-light controls; z, w output blends
#endif
} fc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(set = 1, binding = 0) uniform samplerCube cubes[16];

layout(location = 0) in vec4 inNormal;
layout(location = 1) in vec4 inUvFog;
layout(location = 2) in vec4 inView;
#ifdef FX_SHADOW
layout(location = 3) in vec4 inShadow;
layout(location = 4) in vec4 inOcclusion;
#endif
layout(location = 5) in vec4 inRim;
layout(location = 6) in vec4 inTangent;
layout(location = 7) in vec4 inTint;
#ifdef FX_DEBUG
layout(location = 8) in vec4 inSpecular;
#endif
layout(location = 9) in vec4 inExtra;
layout(location = 0) out vec4 outColor;

#ifdef FX_CHANGE_COLOR
// Team colours: sixteen mask channels, in order. The first channel that is not exactly zero replaces the texture colour
// with its own colour, scaled by the mask.
vec3 skin_change_color(vec3 base, vec2 uv) {
  vec4 masks[4] = vec4[4](texture(textures[7], uv), texture(textures[4], uv), texture(textures[14], uv), texture(textures[2], uv));
  for (int i = 15; i >= 0; --i) {
    float mask = masks[i >> 2][i & 3];
    if (mask != 0.0) base = mask * fc.ChangeColor[i].rgb;
  }
  return base;
}
#endif

// The environment's specular at this reflection, as one strength.
float skin_specular(vec3 reflected, float gloss) {
#ifdef FX_CUBE_SPECULAR
  vec3 viewDir = vec3(dot(reflected, fc.View[0].xyz), dot(reflected, fc.View[1].xyz), dot(reflected, fc.View[2].xyz));
  vec2 cube = texture(cubes[11], viewDir).xy * fc.DifLghMax.w;
  return gloss * (cube.x * fc.SpecCubPrm1.x * inExtra.x + cube.y * fc.SpecCubPrm1.z);
#else
  float lobeA = 0.0, lobeB = 0.0;
  for (int i = 0; i < 4; ++i) {
    float aligned = max(dot(reflected, fc.DifLghVec[i].xyz), 0.0);
    lobeA += fx_pow(aligned, fc.SpecCubPrm1.y) * fc.DifLghCol[i].w;
    lobeB += fx_pow(aligned, fc.SpecCubPrm1.w) * fc.DifLghCol[i].w;
  }
  return gloss * (fc.SpecCubPrm1.x * lobeA * inExtra.x + fc.SpecCubPrm1.z * lobeB);
#endif
}

#ifdef FX_SWEAT
// The bright sharp highlight on wet skin, off a normal that also carries the sweat bump map.
float skin_sweat_highlight(vec3 reflected) {
#ifdef FX_CUBE_SPECULAR
  vec3 viewDir = vec3(dot(reflected, fc.View[0].xyz), dot(reflected, fc.View[1].xyz), dot(reflected, fc.View[2].xyz));
  return texture(cubes[11], viewDir).z * fc.DifLghMax.w;
#else
  float sum = 0.0;
  for (int i = 0; i < 4; ++i) sum += fx_pow(max(dot(reflected, fc.DifLghVec[i].xyz), 0.0), fc.Sweat.y) * fc.DifLghCol[i].w;
  return sum;
#endif
}
#endif

vec3 skin_bend(vec3 N, vec3 T, vec3 B, vec2 offset) {
  return fx_normalize(N * fc.NormalFlip.z + T * offset.x * fc.NormalFlip.x + B * offset.y * fc.NormalFlip.y);
}

void main() {
  vec2 uv = inUvFog.xy;
  vec3 N = inNormal.xyz;
  vec3 V = inView.xyz;
  vec3 T = inTangent.xyz;
  vec3 B = cross(N, T);

  vec3 base = textureLod(textures[0], uv, fc.Level.w).rgb;
#ifdef FX_LOGO
  vec4 logo = fx_bool(fc.Bools, 150) ? texture(textures[7], uv) : texture(textures[4], uv);
  base = mix(base, logo.rgb, logo.a);
#endif
#ifdef FX_CHANGE_COLOR
  base = skin_change_color(base, uv);
#endif

  vec4 surface = texture(textures[1], uv);             // normal xy, gloss w
#ifdef FX_DEBUG
  float gloss = surface.w * fc.DebugPS0.z;
  vec2 normalMap = (surface.xy - 0.5) * fc.DebugPS0.y * -2.0;
#else
  float gloss = surface.w;
  vec2 normalMap = 1.0 - 2.0 * surface.xy;
#endif
#ifdef FX_MUSCLE
  // A flexing muscle has its own normal map, faded in by its alpha.
  vec4 muscle = texture(textures[2], (uv - fc.MuscleUV.xy) * (1.0 / (fc.MuscleUV.zw - fc.MuscleUV.xy)));
  normalMap = mix(normalMap * mix(1.0, fc.MuscleParam.x, muscle.w), 1.0 - 2.0 * muscle.xy, fc.MuscleParam.y * muscle.w);
#endif
#ifdef FX_WRINKLES
  vec2 wrinkle = texture(textures[5], (uv - fc.WrinkUV.xy) * (1.0 / (fc.WrinkUV.zw - fc.WrinkUV.xy))).yw;
  normalMap += (1.0 - 2.0 * wrinkle) * inUvFog.w;
#endif

  vec3 n = skin_bend(N, T, B, normalMap);
  float nv = dot(n, V);
  vec3 reflected = reflect(V, n);

  float specularGloss = gloss;
  float sweatHighlight = 0.0;
#ifdef FX_BSS
  vec4 wet = texture(textures[14], uv);                // x blood, y sweat, z how glossy
  float wetMask = wet.y * texture(textures[6], uv).w;
  float wetGloss = mix(fc.Param0.w, fc.Param0.z, wet.z);
#ifdef FX_BLOOD_TINT
  wetGloss *= 1.0 - wet.x;
  base *= mix(vec3(1.0), fc.BSSBldClr.rgb, wet.x);
#endif
  specularGloss = mix(gloss, wetGloss, wetMask);
#ifdef FX_SWEAT
  float sweat = fc.Sweat.x * wetMask;
#ifdef FX_BLOOD
  sweat = mix(sweat, wet.x * fc.Blood.x, wet.x);
#endif
  if (sweat > 0.0) {
    vec2 sweatBump = texture(textures[4], uv * fc.Sweat.z).xy * 2.0;
    vec2 wetOffset = normalMap + wetMask * vec2(sweatBump.x - 1.0, 1.0 - sweatBump.y);
#ifdef FX_BLOOD
    // Blood's thickness, sampled around the point, bends the normal at its edges. The steps are fixed by the game, not per texel.
    vec2 d = vec2(1.0 / 2048.0, 1.0 / 1024.0);
    float upRight = texture(textures[14], uv + d).x, downRight = texture(textures[14], uv + vec2(d.x, -d.y)).x;
    float downLeft = texture(textures[14], uv - d).x, upLeft = texture(textures[14], uv + vec2(-d.x, d.y)).x;
    wetOffset += wet.x * vec2(upRight + downRight - downLeft - upLeft, upRight - downRight - downLeft + upLeft);
#endif
    vec3 wetNormal = skin_bend(N, T, B, wetOffset);
    sweatHighlight = skin_sweat_highlight(reflect(V, wetNormal)) * sweat;
  }
#endif
#endif
  vec3 albedo = inTint.rgb * fc.MatDifCol.rgb * base;

  float lit = 1.0;
#ifdef FX_SHADOW
  vec4 shadowMap = textureLod(textures[12], inShadow.xy / inShadow.w, 0.0);
  float amount = ch_shadow_amount(shadowMap, inShadow.z, fc.ShadowPrm[0], fc.ShadowPrm[1], fc.ShadowPrm[2], dot(n, fc.ViewLightDepth.xyz));
  lit = 1.0 - clamp(amount + inOcclusion.x, 0.0, 1.0);
#endif

  // Light arriving at the surface: a blurred cube, its alpha driving a sub-surface tint, and a sky/ground hemisphere.
  vec4 cubeLight = texture(cubes[13], n);
  vec3 cubeDiffuse = min(cubeLight.rgb * fc.DifLghMax.xyz, fc.LightLimit.xyz);
  float scatterAmount = fc.DifLghMaxA.x * cubeLight.a * lit;
  vec3 scatter = mix(fc.SSSSinCol2.xyz, fc.SSS.xyz, scatterAmount) * fc.SSS.w;
  float skyT = 0.5 * dot(-n, fc.SkyDir.xyz) + 0.5;
  vec3 hemisphere = mix(fc.SkyCol.xyz, fc.GroundCol.xyz, skyT) * fc.MatAmbCol.xyz;
  vec3 shadowTint = mix(fc.SSSSinCol2.xyz, vec3(1.0), lit);
  vec3 light = (1.0 - scatter) * cubeDiffuse * lit * shadowTint + scatter * scatterAmount + hemisphere;

  // Above 1.0 the glossy parts lose the excess and bleach towards white instead.
  float blowOut = min(fc.HdLig.x * gloss, 1.0);
  vec3 inRange = min(light, vec3(1.0));
  vec3 color = albedo * (inRange + (light - inRange) * (1.0 - blowOut));
  color = mix(color, vec3(1.0), clamp((light - fc.HdLig.y) * blowOut, 0.0, 1.0));

  // The key light's highlight, only where the screen-space rim map says so.
  float keyAligned = max(dot(fx_normalize(n + V), fc.DifLghVec[4].xyz), 0.0);
  float fresnel = fx_pow(1.0 - abs(nv), fc.Param0.y);
  vec3 keyHighlight = fc.DifLghCol[4].xyz * keyAligned * fresnel * fc.Param0.x * inRim.w;
  vec2 rimMap = texture(textures[15], inRim.xy).xy;
  float rim = fx_pow(max(rimMap.y - inRim.z, 0.0) * fc.RimLightDepth.z + rimMap.x, fc.RimLightParam.x);
  rim *= (1.0 - abs(nv)) * ((gloss - 1.0) * fc.RimLightParam.y + 1.0) * fc.RimLightParam.z;
#ifdef FX_DEBUG
  vec3 highlight = mix(inSpecular.xyz, keyHighlight, fc.DebugPS1.y);
  vec3 rimmed = (color + highlight) * 0.75 + color * (1.0 + highlight) * 0.25;
  color += rim * fc.DebugPS1.x * (mix(color, rimmed, fc.DebugPS1.x) - color);
#else
  color = mix(color, (color + keyHighlight) * 0.75 + color * (1.0 + keyHighlight) * 0.25, rim);
#endif

  // Specular and the reflection of the floor sit on top, screened over what is underneath.
  float groundReflection = max(-reflected.y * fx_pow(1.0 + nv, fc.Reflect.w), 0.0);
  vec3 shine = (fc.SpecCubPrm3.xyz * (skin_specular(reflected, specularGloss) + sweatHighlight) + fc.Reflect.xyz * groundReflection * gloss) * lit;
  color = color * (1.0 - min(shine, vec3(1.0))) + shine;

  color = mix(color, fc.FogColor.xyz, inUvFog.z);
#ifdef FX_DEBUG
  outColor = vec4(mix(vec3(lit), mix(vec3(rim), color, fc.DebugPS1.z), fc.DebugPS1.w), mix(1.0, inTint.w, fc.DebugPS1.z));
#else
  outColor = vec4(color, inTint.w);
#endif
}
#endif

#endif
