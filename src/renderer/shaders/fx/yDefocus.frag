#version 450
// yDefocus: blends the sharp frame (slot 0) with a pre-blurred copy (slot 1) by g_f1DefocusRate, then shifts the hue
// of the result (g_f4AdjustHue_YCbCr: y Cb, z Cr, w amount). Option FX_DEBUG is the debug technique: it also has two
// extra blend stages (overlay rate and dodge rate) between the mix and the hue shift.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 g_f1DefocusRate;
  vec4 g_f4AdjustHue_YCbCr;
#ifdef FX_DEBUG
  vec4 g_DEBUG_fOverlayRate;
  vec4 g_DEBUG_fDodgeRate;
#endif
} fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  vec3 sharp = texture(textures[0], vTexCoord).rgb;
  vec3 blurred = texture(textures[1], vTexCoord).rgb;
  vec3 p = mix(sharp, blurred, fc.g_f1DefocusRate.x);
#ifdef FX_DEBUG
  // Overlay of the mix with itself (dark parts squared up, light parts squared down), blended in by the overlay rate.
  vec3 overlay = mix(1.0 - 2.0 * (1.0 - p) * (1.0 - p), 2.0 * p * p, lessThan(p, vec3(0.5)));
  p += (min(overlay, 1.0) - p) * fc.g_DEBUG_fOverlayRate.x;
  // Colour dodge of that with itself (a value of exactly 1 stays 1), blended in by the dodge rate.
  vec3 dodge = vec3(p.x == 1.0 ? 1.0 : p.x / (1.0 - p.x), p.y == 1.0 ? 1.0 : p.y / (1.0 - p.y), p.z == 1.0 ? 1.0 : p.z / (1.0 - p.z));
  p += (min(dodge, 1.0) - p) * fc.g_DEBUG_fDodgeRate.x;
#endif
  float cb = fc.g_f4AdjustHue_YCbCr.y, cr = fc.g_f4AdjustHue_YCbCr.z;
  // The shift is the colour offset of a Cb / Cr tint around a luma equal to the red channel.
  vec3 tint = vec3(1.402 * cr, -0.71414 * cr - 0.34414 * cb, 1.772 * cb);
  outColor = vec4(p + fc.g_f4AdjustHue_YCbCr.w * (vec3(p.x) + tint - p), 1.0);
}
