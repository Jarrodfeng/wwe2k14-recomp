#version 450
// ySprite fragment stage: a textured screen sprite (UI, text, 2D art). See docs\renderer\effects\ySprite.md.
// Options: FX_BLUR 1 / 2 / 3 (horizontal / vertical / diagonal 15-tap blur), FX_TINT (multiply by the vertex colour),
//          FX_GRADATION (radial colour ramp), FX_HSL (hue / saturation / lightness shift),
//          FX_YUV (+ FX_YUV_ALPHA), FX_CHANNEL_MASK (text channel select).

#if defined(FX_HSL) || defined(FX_BLUR) || defined(FX_GRADATION)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef FX_HSL
  vec4 HSL;
#endif
#if defined(FX_BLUR) || defined(FX_GRADATION)
  vec4 offset_weight[8];
#endif
#ifdef FX_GRADATION
  vec4 GradationPointNum;
  vec4 GradationPointColors[12];
  vec4 GradationPointOffsets[3];
#endif
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) in vec2 vTexCoord;
#ifdef FX_GRADATION
layout(location = 1) in vec2 vGradientCoord;
layout(location = 2) in vec4 vColor;
#elif defined(FX_CHANNEL_MASK)
layout(location = 1) in vec4 vChannelSelect;
layout(location = 2) in vec4 vColor;
layout(location = 3) in float vAlphaMaskMode;
#else
layout(location = 1) in vec4 vColor;
#endif

layout(location = 0) out vec4 outColor;


// ---------------------------------------------------------------- blur
#ifdef FX_BLUR
// offset_weight[0].w is the centre weight; entries 1..7 hold an offset (xy) and a weight (w) used for two taps, one each side.
// The blur direction picks which offset components apply.
vec2 blurStep(vec4 entry) {
#if FX_BLUR == 1
  return vec2(entry.x, 0.0);
#elif FX_BLUR == 2
  return vec2(0.0, entry.y);
#else
  return entry.xy;
#endif
}

vec4 sampleSprite(vec2 uv) {
  vec4 sum = texture(textures[0], uv) * vec4(fc.offset_weight[0].w);
  for (int tap = 1; tap < 8; ++tap) {
    vec2 step = blurStep(fc.offset_weight[tap]);
    sum += (texture(textures[0], uv - step) + texture(textures[0], uv + step)) * vec4(fc.offset_weight[tap].w);
  }
  return sum;
}
#else
vec4 sampleSprite(vec2 uv) { return texture(textures[0], uv); }
#endif

// ---------------------------------------------------------------- gradation
#ifdef FX_GRADATION
float rampOffset(int i) {
  vec4 group = fc.GradationPointOffsets[i >> 2];
  int lane = i & 3;
  return lane == 0 ? group.x : (lane == 1 ? group.y : (lane == 2 ? group.z : group.w));
}

// The ramp colour for index i. The game indexes the constant file directly; an index below zero (a negative point count,
// never produced by the game) lands in the registers just below the colours, which are the blur weights.
vec4 rampColor(int i) {
  if (i >= 12) return vec4(0.0);
  if (i >= 0) return fc.GradationPointColors[i];
  int reg = 12 + i;
  return (reg >= 3 && reg <= 10) ? fc.offset_weight[reg - 3] : vec4(0.0);
}

// Colour from the ramp at distance t from the centre: the first point whose offset is beyond t ends the ramp segment;
// before the first point the first colour holds, past the last the last colour holds.
vec4 rampAt(int count, float t) {
  int hit = -1;
  for (int i = 0; i < min(count, 12); ++i) {
    if (rampOffset(i) > t) { hit = i; break; }
  }
  if (hit >= 1) {
    float lo = rampOffset(hit - 1);
    float along = (t - lo) * (1.0 / (rampOffset(hit) - lo));
    vec4 from = rampColor(hit - 1);
    return along * (rampColor(hit) - from) + from;
  }
  if (hit == 0) return rampColor(0);
  return rampColor(count - 1);
}

// What the sprite colour is multiplied by. No ramp points: the vertex colour (so it ends up applied twice, as in the game).
vec4 gradationFactor() {
  float countF = trunc(fc.GradationPointNum.x);
  int count = int(clamp(countF, -1024.0, 1024.0));
  if (count == 0) return vColor;
  float t = 2.0 * length(vGradientCoord - 0.5);
  return rampAt(count, t) * vColor;
}
#endif

// ---------------------------------------------------------------- hue / saturation / lightness
// The game's min and max are not IEEE: they pick by comparison, so NaN in the first operand gives the second (GAME_RULES_DRAWS F4).
#ifdef FX_HSL
// The per-channel offsets for hue h (0..1, may run past 1): a hand-fitted piecewise-linear hue ramp, returned as (red, green, blue).
vec3 hueRamp(float h) {
  bool above23 = h > 0.666667;
  bool above13 = h > 0.333333;
  // sector values shared by the later selections
  float aZ = above23 ? 4.2 * h - 2.91 : 3.54 * h - 2.47;
  float aX = above23 ? -1.8 * h + 2.09 : 3.54 * h - 1.47;
  float aY = above23 ? -1.8 * h + 1.09 : -2.46 * h + 1.53;
  float bY = above13 ? -0.66 * h - 0.37 : -4.2 * h + 0.81;
  float bZ = above13 ? -0.66 * h + 0.63 : 1.8 * h - 0.19;
  float bW = above13 ? 5.34 * h - 2.37 : 1.8 * h - 1.19;
  vec3 upper = (h > 0.833333) ? vec3(0.66 * h + 0.04, -5.34 * h + 5.04, 0.66 * h - 0.96) : vec3(aZ, aX, aY);
  vec3 middle = (h > 0.5) ? vec3(upper.z, upper.x, upper.y) : vec3(bZ, bY, bW);
  vec3 ramp = middle;
  if (!(h > 0.166667)) ramp = vec3(2.46 * h - 0.3, -3.54 * h + 0.7, -3.54 * h - 0.3);
  return vec3(ramp.y, ramp.x, ramp.z);
}

// Shifts the hue of colour p by hsl.x (turns), adds hsl.y to its chroma and moves its lightness by hsl.z (-1 darker .. +1 lighter).
// Lightness is the weighted channel sum 0.3 / 0.59 / 0.11. Grey pixels (no hue) become flat grey at the new lightness.
vec4 hslAdjust(vec4 p, vec3 hsl) {
  float maxRG = (p.g > p.r) ? p.g : p.r;
  float maxC = max(p.b, maxRG);
  float minC = min(min(p.r, p.g), p.b);
  float delta = maxC - minC;

  float hueDegrees;
  if (p.b > maxRG) hueDegrees = 240.0 + (60.0 / delta) * (p.r - p.g);
  else if (p.g > p.r) hueDegrees = 120.0 + (60.0 / delta) * (p.b - p.r);
  else hueDegrees = (60.0 / delta) * (p.g - p.b);
  if (-hueDegrees > 0.0) hueDegrees += 360.0;
  float hue = hueDegrees * 0.00277778;
  if (maxC == minC) hue = -100.0;
  float shifted = hsl.x + hue;
  float wrapped = (-shifted > 0.0) ? (hsl.x + 1.0) + hue : shifted;
  float hueShifted = (wrapped > 1.0) ? wrapped - 1.0 : wrapped;

  float lum = 0.3 * p.r + 0.59 * p.g + 0.11 * p.b;
  float lightness = (hsl.z >= 0.0) ? (1.0 - lum) * hsl.z + lum : (hsl.z + 1.0) * lum;
  float chroma = (maxC + (hsl.y - minC)) * (1.0 - abs(hsl.z));

  vec3 rgb = vec3(lightness);
  if (!(chroma <= 0.0) && !(hueShifted < 0.0)) {
    vec3 offsets = hueRamp(hueShifted) * chroma;
    // keep the result inside 0..1 by shrinking the offsets around the lightness
    float hi = max(max(offsets.x, offsets.y), offsets.z);
    float lo = min(min(offsets.x, offsets.y), offsets.z);
    float scaleHigh = (hi + lightness > 1.0) ? (1.0 - lightness) * (1.0 / hi) : 1.0;
    float scaleLow = lightness * (1.0 / (-lo));
    float scale = (lo + lightness < 0.0) ? ((scaleHigh > scaleLow) ? scaleLow : scaleHigh) : scaleHigh;
    rgb = offsets * scale + lightness;
  }
  return vec4(rgb, p.a);
}
#endif

void main() {
#if defined(FX_YUV)
  // Planar video: Y, U and V in three textures (BT.601 limited range), optional alpha plane.
  float y = texture(textures[0], vTexCoord).x;
  float u = texture(textures[1], vTexCoord).x;
  float v = texture(textures[2], vTexCoord).x;
  vec3 rgb = vec3(y * 1.164 - 0.87075 + v * 1.596,
                  (y * 1.164 - 0.07275) - (u * 0.391 - 0.1955) - (v * 0.813 - 0.4065),
                  y * 1.164 - 1.08175 + u * 2.018);
#ifdef FX_YUV_ALPHA
  outColor = vec4(rgb, texture(textures[3], vTexCoord).x) * vColor;
#else
  outColor = vec4(rgb * vColor.rgb, vColor.a);
#endif
#elif defined(FX_CHANNEL_MASK)
  vec4 texel = texture(textures[0], vTexCoord);
  float selectSum = vChannelSelect.x + vChannelSelect.y + vChannelSelect.z + vChannelSelect.w;
  if (selectSum != 0.0) {
    float coverage = dot(texel, vChannelSelect);
    if (vAlphaMaskMode == 0.0) {
      // distance-field style channel: above one half it becomes solid and the rim brightens
      bool solid = coverage > 0.5;
      float rim = solid ? 2.0 * coverage - 1.0 : 0.0;
      float edge = solid ? 1.0 : coverage;
      outColor = vec4(rim, rim, rim, edge + edge) * vColor;
    } else {
      // the channel is plain coverage: it becomes the alpha of a flat colour
      outColor = vec4(vColor.rgb, coverage * vColor.a);
    }
  } else {
    outColor = texel * vColor;
  }
#else
  vec4 product = sampleSprite(vTexCoord);
#ifdef FX_GRADATION
  product = product * vColor;
  product = product * gradationFactor();
#elif defined(FX_TINT)
  product = product * vColor;
#endif
#ifdef FX_HSL
  product = hslAdjust(product, fc.HSL.xyz);
#endif
  outColor = product;
#endif
}
