// The arena surface family (yBG_*): both stages of every permutation live here; each effect's .vert / .frag only sets the stage and includes this file.
// See docs\renderer\effects\yBG.md. The stage defines BG_VERTEX or BG_FRAGMENT first.
//
// Options (any combination the solver finds; see the "solve" block of each .fx.json):
//   FX_SHADOW        shadow-map receiver: the vertex stage projects into the light, the pixel stage darkens
//   FX_SPECULAR      one directional highlight: the vertex stage passes a normal and an eye vector, the pixel stage adds the highlight
//   FX_SPEC_FADE     (with FX_SPECULAR) the highlight strength fades with view depth (SpeLodParam)
//   FX_SPEC_MAP      (with FX_SPECULAR) the highlight strength is scaled by the alpha of a specular map
//   FX_ALPHA_SPEC    (with FX_SPECULAR) the diffuse texture's alpha scales the highlight and is not part of the output alpha
//   FX_NOISE         a tiled noise texture perturbs the colour
//   FX_LEAN          (with a light map) no shadow slots between the stages: uv, light-map uv, colour
//   FX_DEAD_SLOTS    (with FX_NO_LM) the two shadow interpolator slots exist but carry zero: a surface that is not a receiver
//   FX_FIXED_LOD     the diffuse is read at a fixed mip level, MipmapBias.x, instead of a biased level
//   FX_CONV_SCREEN   (vertex) the clip position is re-centred and rescaled by ConvScreen (the car-style surfaces)
//   FX_STATIC        fixed scenery: vertices are already in world space (no world matrix)
//   FX_NO_BIAS       the diffuse is read without the mip bias
//   FX_NO_LM         no light map in the pixel stage (and no light-map uv / unused shadow slots between the stages)
//   FX_OVERLAY       a decal texture (slot 14) is blended over the diffuse by its alpha, alpha = max of both
//   FX_PICKUP        a pickup texture (slot 7) is blended over the diffuse by its alpha
//   FX_REFLECT       (with FX_SPECULAR) environment reflection from the cube map in slot 8, highlight added after the light map (Reflect)
//   FX_REFLECT_ADD   (with FX_REFLECT) the reflection is added to the base colour, not blended over it
//   FX_REFLECT_MASK  (with FX_REFLECT) a mask texture (slot 9) scales the reflection
//   FX_EDGE_FADE     (vertex) alpha falls to zero where the surface is seen at a grazing angle (EyePos)
//   FX_PLANAR_UV     (vertex, scrolling) no uv attribute: uv = world x-z times SL_Scale
//   FX_PLANAR_YZ     (with FX_PLANAR_UV) the projection is onto the y-z plane instead of x-z
//   FX_VERTEX_LM     the light map is read in the vertex stage (per-vertex) instead of per pixel
#ifndef FX_BG_GLSL
#define FX_BG_GLSL

// ---- Interpolator layout. Without a highlight: uv, [noise uv], shadow, shadow bias, light-map uv, colour.
// With a highlight: uv(+fade), normal, eye vector, shadow, shadow bias, light-map uv, colour.
#ifdef FX_SCROLL
// Scrolling surfaces: one uv per diffuse layer (1 to 4), then the light-map uv (unless FX_NO_LM), then the colour.
#ifdef FX_LAYER4
#define BG_LAYER_COUNT 4
#elif defined(FX_LAYER3)
#define BG_LAYER_COUNT 3
#elif defined(FX_LAYER2)
#define BG_LAYER_COUNT 2
#else
#define BG_LAYER_COUNT 1
#endif
#ifdef FX_NO_LM
#define BG_LOC_COLOR BG_LAYER_COUNT
#else
// Literal numbers (the proof harness cannot read a sum in a location qualifier).
#if BG_LAYER_COUNT == 4
#define BG_LOC_LM 4
#define BG_LOC_COLOR 5
#elif BG_LAYER_COUNT == 3
#define BG_LOC_LM 3
#define BG_LOC_COLOR 4
#elif BG_LAYER_COUNT == 2
#define BG_LOC_LM 2
#define BG_LOC_COLOR 3
#else
#define BG_LOC_LM 1
#define BG_LOC_COLOR 2
#endif
#endif
#elif defined(FX_NO_LM)
// No light map: the shadow slots exist only for a shadow receiver (or as dead slots, FX_DEAD_SLOTS), and there is no light-map uv.
#if defined(FX_SHADOW) || defined(FX_DEAD_SLOTS)
#define BG_SLOTS
#endif
#ifdef BG_SLOTS
#define BG_SHADOW_IO
#endif
#ifdef FX_SPECULAR
#define BG_LOC_NORMAL 1
#define BG_LOC_EYE 2
#ifdef BG_SLOTS
#define BG_LOC_SHADOW 3
#define BG_LOC_BIAS 4
#define BG_LOC_COLOR 5
#else
#define BG_LOC_COLOR 3
#endif
#define BG_LOC_NOISE 6
#else
#ifdef FX_NOISE
#define BG_LOC_NOISE 1
#ifdef BG_SLOTS
#define BG_LOC_SHADOW 2
#define BG_LOC_BIAS 3
#define BG_LOC_COLOR 4
#else
#define BG_LOC_COLOR 2
#endif
#else
#ifdef BG_SLOTS
#define BG_LOC_SHADOW 1
#define BG_LOC_BIAS 2
#define BG_LOC_COLOR 3
#else
#define BG_LOC_COLOR 1
#endif
#endif
#endif
#elif defined(FX_LEAN) && !defined(FX_SHADOW) && !defined(FX_SPECULAR) && !defined(FX_NOISE)
// Light map but no shadow slots: uv, light-map uv, colour.
#define BG_LOC_LM 1
#define BG_LOC_COLOR 2
#else
#define BG_SHADOW_IO
#ifdef FX_SPECULAR
#define BG_LOC_NORMAL 1
#define BG_LOC_EYE 2
#define BG_LOC_SHADOW 3
#define BG_LOC_BIAS 4
#define BG_LOC_LM 5
#define BG_LOC_COLOR 6
#define BG_LOC_NOISE 7      // not a combination the game uses; keeps the file valid for the solver
#else
#ifdef FX_NOISE
#define BG_LOC_NOISE 1
#define BG_LOC_SHADOW 2
#define BG_LOC_BIAS 3
#define BG_LOC_LM 4
#define BG_LOC_COLOR 5
#else
#define BG_LOC_SHADOW 1
#define BG_LOC_BIAS 2
#define BG_LOC_LM 3
#define BG_LOC_COLOR 4
#endif
#endif
#endif

// ---- Shadow darkening. map: the shadow texture at the receiver (x near depth, y far depth, z a second flat layer).
// range: x start, y end, z bias, w strength. soft: x flat-layer strength, z near slope, w near power. far: x start, y slope, z power, w weight.
float bg_shadow_amount(vec4 map, float receiverZ, float bias, vec4 range, vec4 soft, vec4 farTerm) {
  float receiver = clamp((receiverZ - range.x) / (range.y - range.x) - range.z, 0.0, 1.0);
  vec2 gap = vec2(receiver) - map.xy;
  float nearPart = 1.0 - fx_pow(1.0 - clamp(gap.x * soft.z, 0.0, 1.0), soft.w);
  float farPart = fx_pow(1.0 - clamp((gap.y - farTerm.x) * farTerm.y, 0.0, 1.0), farTerm.z);
  float layer = nearPart + (nearPart * farPart - nearPart) * farTerm.w;
  float keep = 1.0 - bias;
  return max(range.w * keep * layer, keep * soft.x * (1.0 - map.z));
}

#ifdef BG_BUMP
// =====================================================================================================================
// Normal-mapped surfaces (BumpMap, BumpMapLM). The vertex stage builds a tangent frame from the normal alone (there is no tangent
// attribute) and sends colour unlit; all lighting happens per pixel with the normal map (slot 4). Interpolators:
// 0 uv (+ highlight fade in z), 1 eye vector, 2 inverted normal, 3 tangent, 4 bitangent, 5 shadow position, 6 unused (zero),
// then (with a light map) 7 light-map uv, and the colour last (7 without a light map, 8 with one).
// Options: FX_SHADOW, FX_SPEC_FADE, FX_VERTEX_LM (light map read per vertex), FX_SPEC_MAP, FX_SPEC_BUMP_ALPHA, FX_ALPHA_OUT (diffuse alpha is the output alpha), FX_NO_LM (the plain BumpMap).
#ifdef FX_NO_LM
#define BUMP_LOC_COLOR 7
#else
#define BUMP_LOC_LM 7
#define BUMP_LOC_COLOR 8
#endif

// 1 / length from a squared length; a zero-length normal gives zero, not infinity.
float bg_inv_length(float lengthSq) { return lengthSq > 0.0 ? inversesqrt(lengthSq) : 0.0; }

#ifdef BG_VERTEX
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrixArray[3];
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 MdlAlpha;
  vec4 EyePos;
#if !defined(FX_NO_LM) || defined(FX_VERTEX_LM)
  vec4 LightMapParam;         // xy world-to-light-map scale, z gain
#endif
#ifdef FX_SPEC_FADE
  vec4 SpeLodParam;           // x fade start, y fade end (clip depth)
#endif
#ifdef FX_SHADOW
  vec4 TexProjLight[4];       // rows 0, 1 and 3 project into the shadow map
  vec4 ViewLight[3];          // row 2 gives the light depth
#endif
} vc;

layout(set = 1, binding = 1) uniform sampler2D textures[4];   // textures[0] is the game's vertex fetch slot 16 (the light map)

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUv;

layout(location = 0) out vec3 outUv;
layout(location = 1) out vec3 outEye;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec3 outTangent;
layout(location = 4) out vec3 outBitangent;
layout(location = 5) out vec4 outShadow;
layout(location = 6) out vec2 outUnused;
#ifndef FX_NO_LM
layout(location = BUMP_LOC_LM) out vec2 outLm;
#endif
layout(location = BUMP_LOC_COLOR) out vec4 outColor;

void bg_vertex_main() {
  vec3 world = vec3(dot(vc.WorldMatrixArray[0], inPosition), dot(vc.WorldMatrixArray[1], inPosition), dot(vc.WorldMatrixArray[2], inPosition));
  vec3 n = vec3(dot(inNormal, vc.WorldMatrixArray[0].xyz), dot(inNormal, vc.WorldMatrixArray[1].xyz), dot(inNormal, vc.WorldMatrixArray[2].xyz));
  vec4 world4 = vec4(world, 1.0);
  gl_Position = vec4(dot(vc.ViewProj[0], world4), dot(vc.ViewProj[1], world4), dot(vc.ViewProj[2], world4), dot(vc.ViewProj[3], world4));

  // Tangent frame from the normal alone: the axis the normal leans on least picks the sideways direction.
  vec3 tangent;
  vec3 bitangent;
  if (abs(n.y) < 0.5) {
    vec2 side = vec2(n.z, -n.x) * bg_inv_length(n.x * n.x + n.z * n.z);
    tangent = vec3(side.x, 0.0, side.y);
    bitangent = vec3(-side.y * n.y, side.y * n.x - side.x * n.z, side.x * n.y);
  } else {
    float inv = bg_inv_length(n.y * n.y + n.z * n.z);
    tangent = vec3(n.y * n.y + n.z * n.z, -n.x * n.y, -n.x * n.z) * inv;
    bitangent = vec3(0.0, n.z, -n.y) * inv;
  }

  float alpha = vc.MdlAlpha.x * vc.MatAmbCol.w * vc.MatDifCol.w;
#ifdef FX_VERTEX_LM
  vec2 lmUv = world.xz * vc.LightMapParam.xy * 0.5 + 0.5;
  vec3 lm = textureLod(textures[0], lmUv, 0.0).xyz * vc.LightMapParam.z;
  outColor = vec4(lm * inColor.xyz, alpha * inColor.w);
#else
  outColor = vec4(inColor.xyz, alpha * inColor.w);
#endif
#ifndef FX_NO_LM
  outLm = world.xz * vc.LightMapParam.xy * 0.5 + 0.5;
#endif
#ifdef FX_SPEC_FADE
  outUv = vec3(inUv, 1.0 - clamp((gl_Position.z - vc.SpeLodParam.x) / (vc.SpeLodParam.y - vc.SpeLodParam.x), 0.0, 1.0));
#else
  outUv = vec3(inUv, 1.0);
#endif
  outEye = vc.EyePos.xyz - world;
  outNormal = -n;
  outTangent = tangent;
  outBitangent = bitangent;
#ifdef FX_SHADOW
  outShadow = vec4(dot(vc.TexProjLight[0], world4), dot(vc.TexProjLight[1], world4), dot(vc.ViewLight[2], world4), dot(vc.TexProjLight[3], world4));
#else
  outShadow = vec4(0.0);
#endif
  outUnused = vec2(0.0);
}
#endif

#ifdef BG_FRAGMENT
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifndef FX_NO_LM
  vec4 LightMapParam;         // z gain
#endif
  vec4 MipmapBias;            // x: the diffuse is read at this fixed mip level
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
  vec4 vMin;
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 SpecularCol;
  vec4 SpecularLev;
  vec4 iSpecularPow;
#ifdef FX_SHADOW
  vec4 ShadowPrm[3];
  vec4 ViewLight[3];
#endif
} fc;

layout(location = 0) in vec3 inUv;
layout(location = 1) in vec3 inEye;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inTangent;
layout(location = 4) in vec3 inBitangent;
#ifdef FX_SHADOW
layout(location = 5) in vec4 inShadow;
#endif
#ifndef FX_NO_LM
layout(location = BUMP_LOC_LM) in vec2 inLm;
#endif
layout(location = BUMP_LOC_COLOR) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void bg_fragment_main() {
  vec4 diffuse = textureLod(textures[0], inUv.xy, fc.MipmapBias.x);
  vec4 normalMap = texture(textures[4], inUv.xy);
  vec3 mapped = normalMap.xyz * 2.0 - 1.0;
  // The normal map bends the (inverted) surface normal along the tangent frame.
  vec3 inward = fx_normalize(inNormal * mapped.z + inTangent * mapped.x - inBitangent * mapped.y);

  vec3 diffuseLight = vec3(0.0);
  for (int i = 0; i < 4; ++i) {
    diffuseLight += max(dot(-inward, fc.DifLghVec[i].xyz), fc.vMin[i]) * fc.DifLghCol[i].xyz;
  }
  vec3 hemisphere = mix(fc.SkyCol.xyz, fc.GroundCol.xyz, 0.5 * dot(inward, fc.SkyDir.xyz) + 0.5);
  vec3 lighting = diffuseLight * fc.MatDifCol.xyz + hemisphere * fc.MatAmbCol.xyz;

  vec3 halfway = fx_normalize(fx_normalize(inEye) - fc.DifLghVec[0].xyz);
#ifdef FX_SPEC_BUMP_ALPHA
  float level = fc.SpecularLev.x * normalMap.w;   // the highlight strength rides in the normal map's alpha
#elif defined(FX_ALPHA_OUT)
  float level = fc.SpecularLev.x;   // the diffuse alpha goes to the output instead
#else
  float level = fc.SpecularLev.x * diffuse.w;
#endif
#ifdef FX_SPEC_MAP
  level *= texture(textures[5], inUv.xy).w;
#endif
  vec3 specular = fc.SpecularCol.xyz * level * fx_pow(max(dot(inward, halfway), 0.0), fc.iSpecularPow.x) * inUv.z;

  vec3 color = diffuse.xyz * inColor.xyz * lighting + specular;
#ifdef FX_SHADOW
  vec4 map = textureLod(textures[12], inShadow.xy / inShadow.w, 0.0);
  float bias = fc.ShadowPrm[1].y * (1.0 - max(dot(-inward, fc.ViewLight[2].xyz), 0.0));
  color *= 1.0 - bg_shadow_amount(map, inShadow.z, bias, fc.ShadowPrm[0], fc.ShadowPrm[1], fc.ShadowPrm[2]);
#endif
#ifndef FX_NO_LM
  color *= texture(textures[13], inLm).xyz * fc.LightMapParam.z;
#endif
#ifdef FX_ALPHA_OUT
  outColor = vec4(color, diffuse.w * inColor.w);
#else
  outColor = vec4(color, inColor.w);
#endif
}
#endif
#endif

#if defined(BG_VERTEX) && !defined(BG_BUMP)
// =====================================================================================================================
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ViewProj[4];
  vec4 WorldMatrixArray[3];   // the object's 4x3 world matrix (rows)
  vec4 MatAmbCol;
  vec4 MatDifCol;
  vec4 MdlAlpha;
  vec4 DifLghCol[4];
  vec4 DifLghVec[4];
  vec4 vMin;                  // the floor of each light's n.l
  vec4 SkyDir;
  vec4 SkyCol;
  vec4 GroundCol;
  vec4 LightMapParam;         // xy world-to-light-map scale, z gain
#ifdef FX_CONV_SCREEN
  vec4 ConvScreen;
#endif
#if defined(FX_SPECULAR) || defined(FX_EDGE_FADE)
  vec4 EyePos;
#endif
#ifdef FX_SPEC_FADE
  vec4 SpeLodParam;           // x fade start, y fade end (clip depth)
#endif
#ifdef FX_SHADOW
  vec4 ShadowPrm[2];
  vec4 TexProjLight[4];       // rows 0, 1 and 3 project into the shadow map
  vec4 ViewLight[3];          // row 2 gives the light depth and direction
#endif
#ifdef FX_NOISE
  vec4 Noise;                 // xy world-to-noise scale
#endif
#ifdef FX_PLANAR_UV
  vec4 SL_Scale;              // x world-to-uv scale
#endif
#ifdef FX_SCROLL
  vec4 Time;                  // x seconds
  vec4 UVScroll0;             // xy layer 0 speed, zw layer 1 speed (hundredths of a uv per second)
#if BG_LAYER_COUNT >= 3
  vec4 UVScroll1;             // xy layer 2 speed, zw layer 3 speed
#endif
#endif
} vc;

layout(set = 1, binding = 1) uniform sampler2D textures[4];   // vertex stage: textures[0] is the game's vertex fetch slot 16 (the light map)

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
#ifndef FX_PLANAR_UV
layout(location = 3) in vec2 inUv;
#endif

#ifdef FX_SCROLL
layout(location = 0) out vec2 outUv0;
#if BG_LAYER_COUNT >= 2
layout(location = 1) out vec2 outUv1;
#endif
#if BG_LAYER_COUNT >= 3
layout(location = 2) out vec2 outUv2;
#endif
#if BG_LAYER_COUNT >= 4
layout(location = 3) out vec2 outUv3;
#endif
#elif defined(FX_SPECULAR)
layout(location = 0) out vec3 outUv;
layout(location = BG_LOC_NORMAL) out vec3 outNormal;
layout(location = BG_LOC_EYE) out vec3 outEye;
#else
layout(location = 0) out vec2 outUv;
#endif
#ifdef FX_NOISE
layout(location = BG_LOC_NOISE) out vec2 outNoise;
#endif
#ifdef BG_SHADOW_IO
layout(location = BG_LOC_SHADOW) out vec4 outShadow;
layout(location = BG_LOC_BIAS) out vec3 outBias;
#endif
#ifndef FX_NO_LM
layout(location = BG_LOC_LM) out vec2 outLm;
#endif
layout(location = BG_LOC_COLOR) out vec4 outColor;

void bg_vertex_main() {
#ifdef FX_STATIC
  // Fixed scenery: the vertices are already in world space.
  vec3 world = inPosition.xyz;
  vec3 n = inNormal;
#else
  vec3 world = vec3(dot(vc.WorldMatrixArray[0], inPosition), dot(vc.WorldMatrixArray[1], inPosition), dot(vc.WorldMatrixArray[2], inPosition));
  vec3 n = vec3(dot(inNormal, vc.WorldMatrixArray[0].xyz), dot(inNormal, vc.WorldMatrixArray[1].xyz), dot(inNormal, vc.WorldMatrixArray[2].xyz));
#endif
  vec4 world4 = vec4(world, 1.0);
#ifdef FX_PLANAR_UV
  // No uv attribute: the texture is projected straight down onto the world's x-z plane.
#ifdef FX_PLANAR_YZ
  vec2 inUv = world.yz * vc.SL_Scale.x * 0.5 + 0.5;   // projected sideways, onto the y-z plane
#else
  vec2 inUv = world.xz * vc.SL_Scale.x * 0.5 + 0.5;
#endif
#endif
  gl_Position = vec4(dot(vc.ViewProj[0], world4), dot(vc.ViewProj[1], world4), dot(vc.ViewProj[2], world4), dot(vc.ViewProj[3], world4));
#ifdef FX_CONV_SCREEN
  // Car-style surfaces: the screen position is re-centred and rescaled in clip space (ConvScreen: xy offset, w scale).
  gl_Position.xy = (gl_Position.xy - vc.ConvScreen.xy * gl_Position.w) * vc.ConvScreen.w;   // the same as re-centring after the divide, without dividing
#endif

  // Four directional lights, each floored by vMin, plus a sky-to-ground hemisphere along the (inverted) normal.
  vec3 diffuse = vec3(0.0);
  for (int i = 0; i < 4; ++i) {
    diffuse += max(dot(n, vc.DifLghVec[i].xyz), vc.vMin[i]) * vc.DifLghCol[i].xyz;
  }
  float skyT = 0.5 * dot(-n, vc.SkyDir.xyz) + 0.5;
  vec3 hemisphere = mix(vc.SkyCol.xyz, vc.GroundCol.xyz, skyT);
  vec3 lighting = diffuse * vc.MatDifCol.xyz + hemisphere * vc.MatAmbCol.xyz;
  float alpha = vc.MdlAlpha.x * vc.MatAmbCol.w * vc.MatDifCol.w;
#ifdef FX_EDGE_FADE
  // Edge fade: the surface turns clear when seen at a grazing angle (facing below a third) and opaque face-on.
  float facing = abs(dot(fx_normalize(n), fx_normalize(vc.EyePos.xyz - world)));
  float graze = 1.0 - max(1.5 * facing - 0.5, 0.0);
  alpha *= 1.0 - graze * graze;
#endif

  // The light map lies over the world's x-z plane.
  vec2 lmUv = world.xz * vc.LightMapParam.xy * 0.5 + 0.5;
#ifndef FX_NO_LM
  outLm = lmUv;
#endif

#ifdef FX_VERTEX_LM
  vec3 lm = textureLod(textures[0], lmUv, 0.0).xyz * vc.LightMapParam.z;
  outColor = vec4(lm * inColor.xyz * lighting, alpha * inColor.w);
#else
  outColor = vec4(lighting, alpha) * inColor;
#endif

#ifdef FX_SCROLL
  // Each diffuse layer drifts across the surface at its own speed.
  outUv0 = inUv + vc.UVScroll0.xy * 0.01 * vc.Time.x;
#if BG_LAYER_COUNT >= 2
  outUv1 = inUv + vc.UVScroll0.zw * 0.01 * vc.Time.x;
#endif
#if BG_LAYER_COUNT >= 3
  outUv2 = inUv + vc.UVScroll1.xy * 0.01 * vc.Time.x;
#endif
#if BG_LAYER_COUNT >= 4
  outUv3 = inUv + vc.UVScroll1.zw * 0.01 * vc.Time.x;
#endif
#elif defined(FX_SPECULAR)
#ifdef FX_SPEC_FADE
  float fade = 1.0 - clamp((gl_Position.z - vc.SpeLodParam.x) / (vc.SpeLodParam.y - vc.SpeLodParam.x), 0.0, 1.0);
  outUv = vec3(inUv, fade);
#elif defined(FX_UV_PLAIN)
  outUv = vec3(inUv, 0.0);   // the third component is not written
#else
  outUv = vec3(inUv, 1.0);
#endif
  outNormal = -n;
  outEye = vc.EyePos.xyz - world;
#else
  outUv = inUv;
#endif
#ifdef FX_NOISE
  outNoise = world.xz * vc.Noise.xy;
#endif

#ifdef FX_SHADOW
  outShadow = vec4(dot(vc.TexProjLight[0], world4), dot(vc.TexProjLight[1], world4), dot(vc.ViewLight[2], world4), dot(vc.TexProjLight[3], world4));
  outBias = vec3(0.0, 0.0, vc.ShadowPrm[1].y * (1.0 - max(dot(n, vc.ViewLight[2].xyz), 0.0)));
#elif defined(BG_SHADOW_IO)
  outShadow = vec4(0.0);
  outBias = vec3(0.0);
#endif
}
#endif

#if defined(BG_FRAGMENT) && !defined(BG_BUMP)
// =====================================================================================================================
layout(set = 1, binding = 0) uniform sampler2D textures[16];
#ifdef FX_REFLECT
layout(set = 1, binding = 0) uniform samplerCube cubes[16];
#endif
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 LightMapParam;         // z gain
  vec4 MipmapBias;            // x bias of the diffuse fetch
#ifdef FX_SPECULAR
  vec4 DifLghVec;             // the first light's direction
  vec4 SpecularCol;
  vec4 SpecularLev;
  vec4 iSpecularPow;
#endif
#ifdef FX_SHADOW
  vec4 ShadowPrm[3];
#endif
#ifdef FX_NOISE
  vec4 Noise;                 // z amplitude
#endif
#ifdef FX_REFLECT
  vec4 ReflectAlpha;          // x strength of the environment reflection
#endif
} fc;

#ifdef FX_SCROLL
layout(location = 0) in vec2 inUv0;
#if BG_LAYER_COUNT >= 2
layout(location = 1) in vec2 inUv1;
#endif
#if BG_LAYER_COUNT >= 3
layout(location = 2) in vec2 inUv2;
#endif
#if BG_LAYER_COUNT >= 4
layout(location = 3) in vec2 inUv3;
#endif
#elif defined(FX_SPECULAR)
layout(location = 0) in vec3 inUv;
layout(location = BG_LOC_NORMAL) in vec3 inNormal;
layout(location = BG_LOC_EYE) in vec3 inEye;
#else
layout(location = 0) in vec2 inUv;
#endif
#ifdef FX_NOISE
layout(location = BG_LOC_NOISE) in vec2 inNoise;
#endif
#ifdef FX_SHADOW
layout(location = BG_LOC_SHADOW) in vec4 inShadow;
layout(location = BG_LOC_BIAS) in vec3 inBias;
#endif
#ifndef FX_NO_LM
layout(location = BG_LOC_LM) in vec2 inLm;
#endif
layout(location = BG_LOC_COLOR) in vec4 inColor;
layout(location = 0) out vec4 outColor;

#ifdef FX_SCROLL
// One layer laid over another: straight colour, alpha accumulates; an empty result is black and clear.
vec4 bg_layer_over(vec4 under, vec4 top) {
  float alpha = (1.0 - top.w) * under.w + top.w;
  vec3 weighted = under.xyz * under.w * (1.0 - top.w) + top.xyz * top.w;
  return alpha == 0.0 ? vec4(0.0) : vec4(weighted / alpha, alpha);
}
// A layer for the car-style surfaces: laid over by its alpha, then the CAA logo over that.
vec3 bg_layer_logo(vec3 under, vec4 layer, vec4 logo) {
  vec3 laid = mix(under, layer.xyz, layer.w);
  return mix(laid, logo.xyz, logo.w);
}
#endif

void bg_fragment_main() {
#ifdef FX_SCROLL
  vec4 diffuse = texture(textures[0], inUv0);
#ifdef FX_CAA
#if BG_LAYER_COUNT >= 2
  diffuse.xyz = bg_layer_logo(diffuse.xyz, texture(textures[1], inUv1), texture(textures[14], inUv1));
#endif
#if BG_LAYER_COUNT >= 3
  diffuse.xyz = bg_layer_logo(diffuse.xyz, texture(textures[2], inUv2), texture(textures[14], inUv2));
#endif
#if BG_LAYER_COUNT >= 4
  diffuse.xyz = bg_layer_logo(diffuse.xyz, texture(textures[3], inUv3), texture(textures[14], inUv3));
#endif
#else
#if BG_LAYER_COUNT >= 2
  diffuse = bg_layer_over(diffuse, texture(textures[1], inUv1));
#endif
#if BG_LAYER_COUNT >= 3
  diffuse = bg_layer_over(diffuse, texture(textures[2], inUv2));
#endif
#if BG_LAYER_COUNT >= 4
  diffuse = bg_layer_over(diffuse, texture(textures[3], inUv3));
#endif
#endif
#elif defined(FX_FIXED_LOD)
  vec4 diffuse = textureLod(textures[0], inUv.xy, fc.MipmapBias.x);
#elif defined(FX_NO_BIAS)
  vec4 diffuse = texture(textures[0], inUv.xy);
#else
  vec4 diffuse = fx_texture_bias(textures[0], inUv.xy, fc.MipmapBias.x);
#endif
#ifdef FX_OVERLAY
  // A decal (the CAA logo, slot 14) laid over the diffuse by its own alpha; the alpha keeps the larger of the two.
  vec4 decal = texture(textures[14], inUv.xy);
  diffuse = vec4(mix(diffuse.xyz, decal.xyz, decal.w), max(diffuse.w, decal.w));
#endif
#ifdef FX_PICKUP
  // A pickup texture (slot 7) laid over the diffuse by its own alpha; the alpha stays the diffuse's.
  vec4 pickup = texture(textures[7], inUv.xy);
  diffuse.xyz = mix(diffuse.xyz, pickup.xyz, pickup.w);
#endif
  vec3 color = diffuse.xyz * inColor.xyz;
#if defined(FX_REFLECT) && defined(FX_SHADOW)
  // On a reflective surface only the base colour is shadowed, before the reflection is blended in.
  vec4 map = textureLod(textures[12], inShadow.xy / inShadow.w, 0.0);
  color *= 1.0 - bg_shadow_amount(map, inShadow.z, inBias.z, fc.ShadowPrm[0], fc.ShadowPrm[1], fc.ShadowPrm[2]);
#endif
#ifdef FX_REFLECT
  // Environment reflection (slot 8): the eye vector mirrored about the normal, blended in with a Schlick fresnel weight.
  vec3 reflectNormal = fx_normalize(inNormal);
  vec3 toEye = fx_normalize(inEye);
  float facing = dot(reflectNormal, toEye);
  vec3 mirrored = 2.0 * facing * reflectNormal - toEye;
  float fresnel = 0.2 + 0.8 * pow(1.0 - max(facing, 0.0), 4.0);
  float weight = fresnel * fc.ReflectAlpha.x;
#ifdef FX_REFLECT_MASK
  weight *= texture(textures[9], inUv.xy).x;   // a mask texture scales the reflection
#endif
#ifdef FX_REFLECT_ADD
  color += texture(cubes[8], mirrored).xyz * weight;   // added on top instead of blended
#else
  color = mix(color, texture(cubes[8], mirrored).xyz, weight);
#endif
#endif
#ifdef FX_NOISE
  color = max(color + (texture(textures[6], inNoise).xyz * 2.0 - 1.0) * fc.Noise.z, 0.0);
#endif
#ifdef FX_SPECULAR
  vec3 n = fx_normalize(inNormal);
  vec3 h = fx_normalize(fx_normalize(inEye) - fc.DifLghVec.xyz);
  float level = fc.SpecularLev.x;
#ifdef FX_ALPHA_SPEC
  level *= diffuse.w;
#endif
#ifdef FX_SPEC_MAP
  level *= texture(textures[5], inUv.xy).w;
#endif
  vec3 specular = fc.SpecularCol.xyz * level * fx_pow(max(dot(n, h), 0.0), fc.iSpecularPow.x)
#ifndef FX_REFLECT
    * inUv.z
#endif
  ;
#ifndef FX_REFLECT
  color += specular;
#endif
#endif
#if defined(FX_SHADOW) && !defined(FX_REFLECT)
  vec4 map = textureLod(textures[12], inShadow.xy / inShadow.w, 0.0);
  color *= 1.0 - bg_shadow_amount(map, inShadow.z, inBias.z, fc.ShadowPrm[0], fc.ShadowPrm[1], fc.ShadowPrm[2]);
#endif
#ifdef FX_NO_LM
  vec3 lm = vec3(1.0);
#else
  vec3 lm = texture(textures[13], inLm).xyz * fc.LightMapParam.z;
#endif
#ifdef FX_REFLECT
  color = color * lm + specular;   // the highlight is not darkened by the light map
#else
  color *= lm;
#endif
#ifdef FX_ALPHA_SPEC
  outColor = vec4(color, inColor.w);
#else
  outColor = vec4(color, diffuse.w * inColor.w);
#endif
}
#endif

#endif
