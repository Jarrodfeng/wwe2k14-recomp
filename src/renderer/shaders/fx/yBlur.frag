#version 450
// Screen blurs and texture merges, pixel stage. See docs\renderer\effects\yBlur.md.
// Options (the counts are binary so the option list stays short):
//   FX_T1, FX_T2     weighted sum of offset reads of texture 0: T1 alone 8 reads, T2 alone 13, both 15.
//   FX_SHADOW_BLUR   contact-hardening shadow blur over 11 reads along two lines (inputs are clip positions from the plane-shadow vertex stage).
//   FX_HOLE_FILL     fills uncovered pixels (alpha <= 0) from 12 neighbours that are not fully covered (alpha-tested two-level blend).
//   FX_SINGLE        texture 0 scaled by one weight.
//   FX_M1, FX_M2, FX_M4   weighted sum of 1 + (M1 + 2 * M2 + 4 * M4) textures at the same coordinate.
//   FX_HIGHPASS      bright-pass filter.
//   FX_FLAG_ANY / FX_FLAG_NOT_ONE   4 reads; white if any read is non-zero / not one, else black (a mask grown by a pixel).
//   FX_MIN_R, FX_MIN_RG, FX_MIN_RGBA, FX_MIN_PAIR   5 reads (the pixel and 4 offsets): x and z are weighted sums, y and w the minimum
//                    (a blur that carries a running minimum, e.g. of depth, next to the blurred value). The PAIR version reads two textures
//                    and carries the maximum of the two in z and w.
#if defined(FX_T1) || defined(FX_T2)
#define FX_TAPS
#endif
#if defined(FX_M1) || defined(FX_M2) || defined(FX_M4)
#define FX_MERGE
#endif

#if defined(FX_T1) && defined(FX_T2)
#define TAP_COUNT 15
#elif defined(FX_T2)
#define TAP_COUNT 13
#else
#define TAP_COUNT 8
#endif

#ifdef FX_MERGE
#if defined(FX_M4)
#if defined(FX_M2) && defined(FX_M1)
#define MERGE_COUNT 8
#elif defined(FX_M2)
#define MERGE_COUNT 7
#elif defined(FX_M1)
#define MERGE_COUNT 6
#else
#define MERGE_COUNT 5
#endif
#else
#if defined(FX_M2) && defined(FX_M1)
#define MERGE_COUNT 4
#elif defined(FX_M2)
#define MERGE_COUNT 3
#else
#define MERGE_COUNT 2
#endif
#endif
#endif

#if defined(FX_FLAG_ANY) || defined(FX_FLAG_NOT_ONE)
#define FX_FLAG
#endif
#if defined(FX_MIN_R) || defined(FX_MIN_RG) || defined(FX_MIN_RGBA) || defined(FX_MIN_PAIR)
#define FX_MINMAX
#endif

#if defined(FX_TAPS) || defined(FX_SHADOW_BLUR) || defined(FX_HOLE_FILL) || defined(FX_SINGLE) || defined(FX_FLAG) || defined(FX_MINMAX) || defined(FX_MERGE) || defined(FX_HIGHPASS)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef FX_SHADOW_BLUR
  vec4 ShadowThreshold;    // x: coverage below this counts as lit
#elif defined(FX_HOLE_FILL)
  vec4 SampleOffsets[12];  // xy of each: the 12 neighbour offsets
  vec4 SampleWeights[13];  // [0].x the centre's weight, [1..12].xyz the neighbours' colour weights (the .x of each also counts into the normaliser)
#elif defined(FX_TAPS)
  vec4 SampleOffsets[TAP_COUNT];
  vec4 SampleWeights[TAP_COUNT];
#elif defined(FX_FLAG)
  vec4 SampleOffsets[2];
#elif defined(FX_MINMAX)
#ifdef FX_MIN_PAIR
  vec4 SampleOffsets[4];
#else
  vec4 SampleOffsets[2];
#endif
  vec4 SampleWeights[2];   // [0] the four tap weights, [1].x the weight of the centre pixel
#elif defined(FX_SINGLE)
  vec4 SampleWeights[1];
#elif defined(FX_MERGE)
  vec4 SampleWeights[MERGE_COUNT];
#else
  vec4 HiPass;     // x: brightness threshold
#endif
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];
#ifdef FX_SHADOW_BLUR
layout(location = 0) in vec4 inCentre;   // clip positions of the blur's centre and of the two far ends of the sample line pair
layout(location = 1) in vec4 inEndA;
layout(location = 2) in vec4 inEndB;
#else
layout(location = 0) in vec2 inTexCoord;
#endif
layout(location = 0) out vec4 outColor;

#ifdef FX_SHADOW_BLUR
// Texture coordinate of a clip position given as (x, y, w).
vec2 clipToUV(vec3 p) {
  float inverseW = 1.0 / p.z;
  return vec2(p.x * (0.5 * inverseW) + 0.5, -(p.y * inverseW) * 0.5 + 0.5);
}
#endif

void main() {
#ifdef FX_SHADOW_BLUR
  // Contact-hardening shadow blur. Two lines of 5 taps run from the centre to two far points (at 0.2 .. 1.0 of the way, in clip space).
  // A first pass reads the 10 taps squeezed to 10% of the way to estimate how covered the pixel is; the second pass reads them at (1 - coverage)
  // of the full spread, so a pixel deep in shadow stays sharp and one at the edge blurs wide. Both passes average 11 reads including the centre.
  vec3 centre = inCentre.xyw;
  vec2 uvCentre = clipToUV(centre);
  vec2 uvRead = uvCentre + 1.0 / 512.0;      // the centre read, and the origin of the offsets
  vec2 uvBase = uvCentre + 1.0 / 256.0;      // where the second pass spreads from
  vec4 centreTap = texture(textures[0], uvRead);
  vec2 tapUV[10];
  for (int k = 0; k < 5; ++k) {
    float along = 0.2 * float(k + 1);
    tapUV[k] = clipToUV(centre + along * (inEndA.xyw - centre));
    tapUV[k + 5] = clipToUV(centre + along * (inEndB.xyw - centre));
  }
  float coverage = centreTap.x;
  for (int k = 0; k < 10; ++k) coverage += texture(textures[0], uvRead - (0.1 * (uvRead - tapUV[k]) + 0.00019531250291)).x;
  float spread = 1.0 - clamp(coverage * 0.0909 - fc.ShadowThreshold.x, 0.0, 1.0);
  vec4 sum = centreTap;
  for (int k = 0; k < 10; ++k) sum += texture(textures[0], uvBase - spread * (uvRead - tapUV[k]));
  outColor = sum * 0.0909;
#elif defined(FX_HOLE_FILL)
  vec2 uv = inTexCoord;
  // A pixel with no coverage (alpha <= 0) is filled with the weighted average of the neighbours that are not fully covered (alpha < 1); covered pixels pass through.
  vec4 centre = texture(textures[0], uv);
  if (centre.w > 0.0) {
    outColor = centre;
  } else {
    vec3 sum = centre.rgb * fc.SampleWeights[0].x;
    float total = fc.SampleWeights[0].x;
    for (int i = 0; i < 12; ++i) {
      vec4 s = texture(textures[0], uv + fc.SampleOffsets[i].xy);
      if (s.w < 1.0) {
        sum += s.rgb * fc.SampleWeights[i + 1].xyz;
        total += fc.SampleWeights[i + 1].x;
      }
    }
    outColor = vec4(sum / total, 1.0);
  }
#else
  vec2 uv = inTexCoord;
#endif
#if defined(FX_SHADOW_BLUR) || defined(FX_HOLE_FILL)
#elif defined(FX_TAPS)
  vec4 sum = vec4(0.0);
  for (int i = 0; i < TAP_COUNT; ++i) sum += texture(textures[0], uv + fc.SampleOffsets[i].xy) * fc.SampleWeights[i];
  outColor = sum;
#elif defined(FX_FLAG)
  vec4 taps = vec4(texture(textures[0], uv + fc.SampleOffsets[0].xy).x, texture(textures[0], uv + fc.SampleOffsets[0].zw).x,
                   texture(textures[0], uv + fc.SampleOffsets[1].xy).x, texture(textures[0], uv + fc.SampleOffsets[1].zw).x);
#ifdef FX_FLAG_NOT_ONE
  taps = 1.0 - taps;
#endif
  outColor = vec4(vec3(any(notEqual(taps, vec4(0.0))) ? 1.0 : 0.0), 1.0);
#elif defined(FX_MINMAX)
  vec2 offsets[4] = vec2[4](fc.SampleOffsets[0].xy, fc.SampleOffsets[0].zw, fc.SampleOffsets[1].xy, fc.SampleOffsets[1].zw);
  vec4 t0 = texture(textures[0], uv + offsets[0]);
  vec4 t1 = texture(textures[0], uv + offsets[1]);
  vec4 t2 = texture(textures[0], uv + offsets[2]);
  vec4 t3 = texture(textures[0], uv + offsets[3]);
  vec4 centre = texture(textures[0], uv);
  vec4 w = fc.SampleWeights[0];
  float wc = fc.SampleWeights[1].x;
#if defined(FX_MIN_R) || defined(FX_MIN_RG)
#ifdef FX_MIN_R
  vec4 lows = vec4(t0.x, t1.x, t2.x, t3.x);
  float lowCentre = centre.x;
#else
  vec4 lows = vec4(t0.y, t1.y, t2.y, t3.y);
  float lowCentre = centre.y;
#endif
  float sum = dot(vec4(t0.x, t1.x, t2.x, t3.x), w) + centre.x * wc;
  float low = min(min(min(lows.x, lows.y), min(lows.z, lows.w)), lowCentre);
  outColor = vec4(sum, low, 1.0, 1.0);
#elif defined(FX_MIN_RGBA)
  vec2 sums = vec2(dot(vec4(t0.x, t1.x, t2.x, t3.x), w), dot(vec4(t0.z, t1.z, t2.z, t3.z), w)) + centre.xz * wc;
  vec2 lows = min(min(min(t0.yw, t1.yw), min(t2.yw, t3.yw)), centre.yw);
  outColor = vec4(sums.x, lows.x, sums.y, lows.y);
#else
  // Two textures read at the same offsets (the second one's taps come from the next two offset pairs).
  vec4 a = vec4(t0.x, t1.x, t2.x, t3.x);
  vec4 b = vec4(texture(textures[1], uv + fc.SampleOffsets[2].xy).x, texture(textures[1], uv + fc.SampleOffsets[2].zw).x,
                texture(textures[1], uv + fc.SampleOffsets[3].xy).x, texture(textures[1], uv + fc.SampleOffsets[3].zw).x);
  float centreB = texture(textures[1], uv).x;
  vec4 m = max(a, b);
  float peak = max(centre.x, centreB);
  float lowA = min(min(min(a.x, a.y), min(a.z, a.w)), centre.x);
  float lowM = min(min(min(m.x, m.y), min(m.z, m.w)), peak);
  outColor = vec4(dot(a, w) + centre.x * wc, lowA, dot(m, w) + peak * wc, lowM);
#endif
#elif defined(FX_SINGLE)
  outColor = texture(textures[0], uv) * fc.SampleWeights[0];
#elif defined(FX_MERGE)
  // One texture per slot, written out: each slot is its own binding in the build, so a slot cannot be picked by a loop counter.
  vec4 sum = texture(textures[0], uv) * fc.SampleWeights[0] + texture(textures[1], uv) * fc.SampleWeights[1];
#if MERGE_COUNT > 2
  sum += texture(textures[2], uv) * fc.SampleWeights[2];
#endif
#if MERGE_COUNT > 3
  sum += texture(textures[3], uv) * fc.SampleWeights[3];
#endif
#if MERGE_COUNT > 4
  sum += texture(textures[4], uv) * fc.SampleWeights[4];
#endif
#if MERGE_COUNT > 5
  sum += texture(textures[5], uv) * fc.SampleWeights[5];
#endif
#if MERGE_COUNT > 6
  sum += texture(textures[6], uv) * fc.SampleWeights[6];
#endif
#if MERGE_COUNT > 7
  sum += texture(textures[7], uv) * fc.SampleWeights[7];
#endif
  outColor = sum;
#else
  vec3 rgb = texture(textures[0], uv).rgb;
  float luma = dot(rgb, vec3(0.2989, 0.5866, 0.1145));
  // What is brighter than the threshold, renormalised by the headroom left above it.
  outColor = vec4(2.0 * rgb * (luma - fc.HiPass.x) / (2.0 - fc.HiPass.x), 1.0);
#endif
}
