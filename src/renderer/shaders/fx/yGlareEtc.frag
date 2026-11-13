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

  vec2 dir = vec2((lSw + lSe) - (lNw + lNe), (lNw + lSw) - (lNe + lSe));
  float reduce = max((lNw + lNe + lSw + lSe) * (0.25 / 8.0), 1.0 / 128.0);
  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * texel;

  vec3 a = 0.5 * (texture(textures[0], uv - dir / 6.0).rgb + texture(textures[0], uv + dir / 6.0).rgb);
  vec3 b = 0.5 * a + 0.25 * (texture(textures[0], uv - dir * 0.5).rgb + texture(textures[0], uv + dir * 0.5).rgb);
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
  color += texture(textures[7], uv).rgb * fc.BloomScale.x + texture(textures[8], uv).rgb * fc.StarScale.x;
#endif
  outColor = vec4(color, 1.0);
#endif
}
