#version 450
// The final image of the frame (glare, bloom, luminance adaptation), pixel stage. See docs\renderer\effects\yGlareEtc.md.
// Every shader smooths edges first, then applies the options in this order:
//   FX_DOF (depth of field), FX_FOG (depth tint), FX_NIGHT (desaturate in the dark), FX_TONEMAP (exposure), FX_BLOOM (glare and star added).
// FX_ADAPT is a different pass: it blends last frame's average luminance towards the new one.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#if defined(FX_ADAPT) || defined(FX_DOF) || defined(FX_FOG) || defined(FX_BLOOM) || defined(FX_TONEMAP)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef FX_ADAPT
  vec4 ElapsedTime;
#else
#ifdef FX_DOF
  vec4 DOFParam;       // x near-blur strength, y far-blur strength, w far-blur limit
  vec4 DOFParam2;      // x near focus depth, y far focus depth, z minimum blend
#endif
#ifdef FX_FOG
  vec4 RGB;            // tint colour
  vec4 Alpha;          // x strength
  vec4 ZAdj;           // x depth where the tint starts
#endif
#ifdef FX_BLOOM
  vec4 BloomScale;
  vec4 StarScale;
#endif
#ifdef FX_TONEMAP
  vec4 MiddleGray;
#endif
#endif
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec4 inUv;
layout(location = 0) out vec4 outColor;

#ifndef FX_ADAPT
#ifdef FX_LITE_FXAA
bool fx_flat_pixel = false;   // S102 R18-B2: set by the flat exit (read only by the MARK twin, a tool-only instrument)
#endif
// Edge smoothing along the local edge direction (a luma-based FXAA), on a source the game assumes is 1280x720.
vec3 smoothEdges(vec2 uv, vec2 corner_unused) {
  // S101 lane 34 review F: the texel of the picture actually read (the game's 1280x720 gives exactly the old constants; a picture at another size, e.g. the
  // viewer's shots or a render scale, gets its own), and the corner tap half a texel up-left of it (the vertex stage's zw carried 0.5 / 1280, 0.5 / 720).
  vec2 texel = 1.0 / vec2(textureSize(textures[0], 0));
  vec2 corner = uv - 0.5 * texel;
  const vec3 lumaWeights = vec3(0.299, 0.587, 0.114);
  vec3 nw = texture(textures[0], corner).rgb;
  vec3 ne = texture(textures[0], corner + vec2(texel.x, 0.0)).rgb;
  vec3 sw = texture(textures[0], corner + vec2(0.0, texel.y)).rgb;
  vec3 se = texture(textures[0], corner + texel).rgb;
  vec3 mid = texture(textures[0], uv).rgb;
  float lNw = dot(nw, lumaWeights), lNe = dot(ne, lumaWeights), lSw = dot(sw, lumaWeights), lSe = dot(se, lumaWeights);
  float lMid = dot(mid, lumaWeights);
  float lMin = min(lMid, min(min(lNw, lNe), min(lSw, lSe)));
  float lMax = max(lMid, max(max(lNw, lNe), max(lSw, lSe)));
#ifdef FX_LITE_FXAA
  // S102 R18-B2 (level 2, a LOOK lever): FXAA 3.11's own early exit. Where the five taps are flat (range under max(1/32, lMax / 8)) the four smoothing taps are not made and the
  // pixel is the centre tap. The console shader has no exit; the pixels it takes differ by a rounding of the smoothing (sheet and census: tools/r18/r18_b2_check.py).
#ifdef FX_PLANT_WIDE
  if (lMax - lMin < 10.0 * max(1.0 / 32.0, lMax * (1.0 / 8.0))) {   // planted twin lite_flat_wide: the threshold 10x
#else
  if (lMax - lMin < max(1.0 / 32.0, lMax * (1.0 / 8.0))) {
#endif
    fx_flat_pixel = true;
    return mid;
  }
#endif

  vec2 dir = vec2((lSw + lSe) - (lNw + lNe), (lNw + lSw) - (lNe + lSe));
  float reduce = max((lNw + lNe + lSw + lSe) * (0.25 / 8.0), 1.0 / 128.0);
  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * texel;

#ifdef FX_LITE_FXAA
  // S102 R18-B2 review F1: these four taps run only where the flat exit above was not taken (non-uniform control flow): explicit level 0, never implicit derivatives (the image is one mip,
  // so the result is the same as texture(); tools/r18/spv_lint_b2.py fails any implicit sample that is not on the unconditional path).
  vec3 a = 0.5 * (textureLod(textures[0], uv - dir / 6.0, 0.0).rgb + textureLod(textures[0], uv + dir / 6.0, 0.0).rgb);
  vec3 b = 0.5 * a + 0.25 * (textureLod(textures[0], uv - dir * 0.5, 0.0).rgb + textureLod(textures[0], uv + dir * 0.5, 0.0).rgb);
#else
  vec3 a = 0.5 * (texture(textures[0], uv - dir / 6.0).rgb + texture(textures[0], uv + dir / 6.0).rgb);
  vec3 b = 0.5 * a + 0.25 * (texture(textures[0], uv - dir * 0.5).rgb + texture(textures[0], uv + dir * 0.5).rgb);
#endif
  float lB = dot(b, lumaWeights);
  return (lB < lMin || lB > lMax) ? a : b;
}
#endif

void main() {
#ifdef FX_ADAPT
  // Last frame's adapted luminance and this frame's, blended by an exponential approach (about 0.55 per second).
  float previous = texture(textures[0], vec2(0.5)).x;
  float current = texture(textures[1], vec2(0.01)).x;
  float follow = 1.0 - exp2(fc.ElapsedTime.x * -0.87439);
  outColor = vec4(vec3(mix(previous, current, follow)), 1.0);
#else
  vec2 uv = inUv.xy;
  vec3 color = smoothEdges(uv, inUv.zw);
#if defined(FX_DOF) || defined(FX_FOG)
  float depth = texture(textures[3], uv).x;
#endif
#ifdef FX_DOF
#ifdef FX_LITE
  // S102 R18-B2 (level 1, exact): the blur of the branch taken is FETCHED only where its weight is not 0; the arithmetic after it is the old one, unchanged (a texel of 0 for a weight of 0:
  // mix(c, 0, 0) is c, and the driver sees the same multiply-add chain, so the picture is byte-equal; a first version that skipped the mix as well moved 2 pixels of 921600 by 1 / 255,
  // the driver fusing the shorter chain differently). A non-finite texel the old code multiplied by 0 would have made NaN; the targets are UNORM / float16 colour, finite.
  // (planted twin lite_dof_always_skip: the texel is always 0.) The moved fetches are explicit level 0 (review F1): `amount` is per pixel, so they sit in non-uniform control flow.
  float behind = 1.0 - depth;
  if (fc.DOFParam2.x > behind) {
    float amount = clamp((fc.DOFParam2.x - behind) * fc.DOFParam.x + fc.DOFParam2.z, fc.DOFParam2.z, 1.0);
#ifdef FX_PLANT_DOF
    vec3 blurred = vec3(0.0);
#else
    vec3 blurred = amount != 0.0 ? textureLod(textures[2], uv, 0.0).rgb : vec3(0.0);
#endif
    color = mix(color, blurred, amount);
  } else {
    float amount = clamp(((1.0 - fc.DOFParam2.y) - depth) * fc.DOFParam.y + fc.DOFParam2.z, fc.DOFParam2.z, fc.DOFParam.w);
#ifdef FX_PLANT_DOF
    vec3 bokeh = vec3(0.0);
#else
    vec3 bokeh = amount != 0.0 ? textureLod(textures[6], uv, 0.0).rgb : vec3(0.0);
#endif
    color = mix(color, bokeh, amount);
  }
#else
  vec3 blurred = texture(textures[2], uv).rgb;
  float behind = 1.0 - depth;
  if (fc.DOFParam2.x > behind) {
    float amount = clamp((fc.DOFParam2.x - behind) * fc.DOFParam.x + fc.DOFParam2.z, fc.DOFParam2.z, 1.0);
    color = mix(color, blurred, amount);
  } else {
    vec3 bokeh = texture(textures[6], uv).rgb;
    float amount = clamp(((1.0 - fc.DOFParam2.y) - depth) * fc.DOFParam.y + fc.DOFParam2.z, fc.DOFParam2.z, fc.DOFParam.w);
    color = mix(color, bokeh, amount);
  }
#endif
#endif
#ifdef FX_FOG
  float start = 1.0 - fc.ZAdj.x;
  color = mix(color, fc.RGB.xyz, fc.Alpha.x * max(start - depth, 0.0) / start);
#endif
#if defined(FX_NIGHT) || defined(FX_TONEMAP)
  float luminance = texture(textures[9], vec2(0.01)).x;
#endif
#ifdef FX_NIGHT
  float grey = dot(color, vec3(0.2125, 0.7154, 0.0721));
  float dark = clamp(1.0 - (luminance * 0.243902 + 0.365854), 0.0, 1.0);
  color = mix(color, grey * vec3(1.05, 0.97, 1.27), dark);
#endif
#ifdef FX_TONEMAP
  vec3 scaled = color * (fc.MiddleGray.x / (luminance + 0.001));
  color = scaled / (scaled + 1.0);
#endif
#ifdef FX_BLOOM
#ifdef FX_LITE
  // S102 R18-B2 (level 1, exact): an image whose scale is 0 is not FETCHED (its texel is taken as 0); the sum is the old expression. (planted twin lite_bloom_skip_live: the glow is never fetched.)
#ifdef FX_PLANT_BLOOM
  vec3 glow = vec3(0.0);
#else
  vec3 glow = fc.BloomScale.x != 0.0 ? textureLod(textures[7], uv, 0.0).rgb : vec3(0.0);
#endif
  vec3 star = fc.StarScale.x != 0.0 ? textureLod(textures[8], uv, 0.0).rgb : vec3(0.0);
  color += glow * fc.BloomScale.x + star * fc.StarScale.x;
#else
  color += texture(textures[7], uv).rgb * fc.BloomScale.x + texture(textures[8], uv).rgb * fc.StarScale.x;
#endif
#endif
  outColor = vec4(color, 1.0);
#ifdef FX_LITE_MARK
  if (fx_flat_pixel) outColor = vec4(1.0, 0.0, 1.0, 1.0);   // S102 R18-B2: tool-only instrument twin (g_combine_lite_plant 4): the pixels the flat exit takes are magenta
#endif
#endif
}
