#version 450
// Session 101 lane 159 (docs\renderer\SHADOW_PLANE.md): the plane shadow's blur, pixel stage. Our own shader, not a game shader.
// A contact-hardening blur over 11 reads along two lines. Two lines of 5 taps run from the centre to two far points (at 0.2 .. 1.0 of the way, in clip space). A first pass reads the 10
// taps squeezed to 10% of the way to estimate how covered the pixel is; the second reads them at (1 - coverage) of the full spread, so a pixel deep in shadow stays sharp and one at the
// edge blurs wide. Both passes average 11 reads including the centre. The kernel is this program's (the taps, the 1/512 and 1/256 offsets, the 0.0909 average); the stream's only input
// is the coverage threshold, a pixel constant the record does not own.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 ShadowThreshold;   // x: coverage below this counts as lit (the stream's)
} fc;
layout(set = 1, binding = 0) uniform sampler2D plane_map;   // the 256 x 256 downscaled mask
layout(location = 0) in vec4 in_centre;   // clip positions of the blur's centre and of the two far ends of the sample line pair
layout(location = 1) in vec4 in_end_a;
layout(location = 2) in vec4 in_end_b;
layout(location = 0) out vec4 out_color;

vec2 clip_to_uv(vec3 p) {
  float inverse_w = 1.0 / p.z;
  return vec2(p.x * (0.5 * inverse_w) + 0.5, -(p.y * inverse_w) * 0.5 + 0.5);
}

void main() {
  vec3 centre = in_centre.xyw;
  vec2 uv_centre = clip_to_uv(centre);
  vec2 uv_read = uv_centre + 1.0 / 512.0;      // the centre read, and the origin of the offsets
  vec2 uv_base = uv_centre + 1.0 / 256.0;      // where the second pass spreads from
  vec4 centre_tap = texture(plane_map, uv_read);
  vec2 tap_uv[10];
  for (int k = 0; k < 5; ++k) {
    float along = 0.2 * float(k + 1);
    tap_uv[k] = clip_to_uv(centre + along * (in_end_a.xyw - centre));
    tap_uv[k + 5] = clip_to_uv(centre + along * (in_end_b.xyw - centre));
  }
  float coverage = centre_tap.x;
  for (int k = 0; k < 10; ++k) coverage += texture(plane_map, uv_read - (0.1 * (uv_read - tap_uv[k]) + 0.00019531250291)).x;
  float spread = 1.0 - clamp(coverage * 0.0909 - fc.ShadowThreshold.x, 0.0, 1.0);
  vec4 sum = centre_tap;
  for (int k = 0; k < 10; ++k) sum += texture(plane_map, uv_base - spread * (uv_read - tap_uv[k]));
  out_color = sum * 0.0909;
}
