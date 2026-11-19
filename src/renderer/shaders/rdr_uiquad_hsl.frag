#version 450
// Session 103 lane I2 (the 2D layer's sprite rows, docs\renderer\REMNANT_MAP.md section 10): the hue-shifted UI sprite's pixel step. Our own shader, not a game shader.
// = rdr_uiquad.frag (the picture in slot 0 times the quad's colour), then the game's hue / chroma / lightness shift on the product (the ySprite effect's FX_HSL option, src\renderer\shaders\fx\ySprite.frag:
// the same arithmetic, so the two agree to the last bit on the same inputs). HSL = the pixel constant c2 the leaf uploads: x the hue shift in turns, y the chroma added, z the lightness (-1 darker .. +1 lighter).
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Color;     // the quad's colour (the game's Color constant)
  vec4 AlphaMix;  // alpha = alpha * x + y (1, 0 = as drawn); z: 1 = apply the stream's alpha test, 0 = skip it
  vec4 HSL;       // the hue-shift constant (xyz)
} fc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(set = 1, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// The per-channel offsets for hue h (0..1, may run past 1): the game's piecewise-linear hue ramp, returned as (red, green, blue).
vec3 hueRamp(float h) {
  bool above23 = h > 0.666667;
  bool above13 = h > 0.333333;
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
    float hi = max(max(offsets.x, offsets.y), offsets.z);
    float lo = min(min(offsets.x, offsets.y), offsets.z);
    float scaleHigh = (hi + lightness > 1.0) ? (1.0 - lightness) * (1.0 / hi) : 1.0;
    float scaleLow = lightness * (1.0 / (-lo));
    float scale = (lo + lightness < 0.0) ? ((scaleHigh > scaleLow) ? scaleLow : scaleHigh) : scaleHigh;
    rgb = offsets * scale + lightness;
  }
  return vec4(rgb, p.a);
}

void main() {
  vec4 texel = texture(picture, in_uv);
  vec4 product = hslAdjust(vec4(texel.rgb * fc.Color.rgb, texel.a * fc.Color.a), fc.HSL.xyz);
  out_color = vec4(product.rgb, product.a * fc.AlphaMix.x + fc.AlphaMix.y);
  int func = int(fx_sys.alpha.y);
  if (func != 7 && fc.AlphaMix.z != 0.0) {
    float a = out_color.a;
    bool pass_test = isnan(a) ? func == 5 : ((a < fx_sys.alpha.x && (func & 1) != 0) || (a == fx_sys.alpha.x && (func & 2) != 0) || (a > fx_sys.alpha.x && (func & 4) != 0));
    if (!pass_test) discard;
  }
}
