#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
// Radial blur, pixel stage. See docs\renderer\effects\yRadialBlur.md.
// Options: FX_FIXED_TAPS: nine reads at fixed offsets from the pixel, weighted (offsets and weights supplied by the game).
//          FX_RADIAL_MAX:  the blur steps from each pixel toward the centre, the step shrinking as the pixel gets farther from the centre on each axis (reaching nothing at MaxLength).
//          FX_RADIAL_RATE: the same walk, but the step grows with the distance beyond a dead zone (MinLength) up to a cap (BlurRateLength).
//          FX_FAN (with one of the two above): the walk does not head straight to the centre but along the nearest of a fan of directions, spaced BlurOpenDegree apart.

#if defined(FX_RADIAL_MAX) || defined(FX_RADIAL_RATE)
#define FX_RADIAL
#endif

layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef FX_FIXED_TAPS
  vec4 SampleOffsetArray[9];
  vec4 SampleWeightArray[9];
#else
  vec4 Width;
  vec4 Height;
  vec4 BlurCount;            // number of reads including the pixel itself (whole part used)
  vec4 CenterPosX;
  vec4 CenterPosY;
  vec4 SampleDistanceMin;
  vec4 SampleDistanceMax;    // the step is a fraction of (Max - Min) texels
#ifdef FX_RADIAL_MAX
  vec4 BlurRateMaxLengthX;
  vec4 BlurRateMaxLengthY;
#else
  vec4 BlurRateMinLengthX;
  vec4 BlurRateMinLengthY;
  vec4 BlurRateLength;       // xy: size of the ramp on each axis
#endif
#ifdef FX_FAN
  vec4 BlurOpenDegree;      // angular size of one fan sector, in degrees
#endif
#endif
} fc;

layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

vec2 unit2(vec2 v) {
  return fx_normalize(vec3(v, 0.0)).xy;
}

void main() {
#ifdef FX_FIXED_TAPS
  vec3 sum = vec3(0.0);
  for (int i = 0; i < 9; ++i) sum += texture(textures[0], inTexCoord + fc.SampleOffsetArray[i].xy).rgb * fc.SampleWeightArray[i].x;
  outColor = vec4(sum, 1.0);
#else
  vec2 uv = inTexCoord;
  vec2 toCentre = vec2(fc.CenterPosX.x, fc.CenterPosY.x) - uv;
  vec2 away = abs(toCentre);
#ifdef FX_RADIAL_MAX
  // 1 at the centre line of each axis, falling to 0 at MaxLength.
  vec2 near = 1.0 - min(away / vec2(fc.BlurRateMaxLengthX.x, fc.BlurRateMaxLengthY.x), 1.0);
  float strength = near.x * near.y;
#else
  vec2 ramp = min(max(away - vec2(fc.BlurRateMinLengthX.x, fc.BlurRateMinLengthY.x), 0.0), fc.BlurRateLength.xy) / fc.BlurRateLength.xy;
  float strength = 0.5 * (ramp.x + ramp.y);
#endif
  vec2 direction = unit2(toCentre);
#ifdef FX_FAN
  // Snap the direction's angle (from the vertical axis) down to a multiple of the sector size; the side follows the pixel.
  float sector = floor(degrees(acos(clamp(direction.y, -1.0, 1.0))) / fc.BlurOpenDegree.x) * fc.BlurOpenDegree.x;
  float snapped = radians(sector);
  direction = vec2((direction.x > 0.0 ? 1.0 : -1.0) * sin(snapped), cos(snapped));
#endif
  float reach = fc.SampleDistanceMax.x - fc.SampleDistanceMin.x;
  vec2 step = direction * reach / vec2(fc.Width.x, fc.Height.x) * strength;
  float count = trunc(fc.BlurCount.x);
  vec3 sum = texture(textures[0], uv).rgb;
  // When the farthest read would overshoot the centre, reads that have passed it fall back to the pixel itself.
  bool guarded = count > 1.0 && dot(direction, unit2(toCentre - step * (count - 1.0))) <= 0.0;
  for (int i = 1; i < 16; ++i) {
#if defined(FX_FAN) && defined(FX_RADIAL_MAX)
    // This version always takes at least one step read, even when BlurCount is 1.
    if (i > 1 && float(i) >= count) break;
#else
    if (float(i) >= count) break;
#endif
    vec2 pos = uv + step * float(i);
    if (guarded && dot(direction, unit2(vec2(fc.CenterPosX.x, fc.CenterPosY.x) - pos)) < 0.0) pos = uv;
    sum += texture(textures[0], pos).rgb;
  }
  outColor = vec4(sum / count, 1.0);
#endif
}
