#version 450
// yRetroFilm: old-film look. Slots: 0 scene, 1 and 2 one-row noise strips read at (0, time), 3 a grain / scratch texture (its alpha channel is used).
// The scene is read with a vertical jitter (the cube of a noise value, scaled by g_Time.w), converted to YCbCr, and the
// luma is replaced by (flicker * luma * g_Range.x + g_Range.y, clamped to g_Range.z .. g_Range.w) * grain. Chroma is kept.
// g_Time: x scrolls the noise strips, y and z scroll the grain texture, w is the jitter strength.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 g_Time;
  vec4 g_Range;
} fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

// CLEAN (settings parity S3, Jarrod 08:1x: a player asked for the 30 Years mode's film filter gone): the scene as it is, no shake,
// flicker or grain. The binder draws this variant in place of the game's when FILM GRAIN (30 YEARS) is off (--wwe2k14_film_grain=false).
void main() {
#ifdef CLEAN
  outColor = vec4(texture(textures[0], vTexCoord).rgb, 1.0);
#else
  float jitterNoise = texture(textures[2], vec2(0.0, fc.g_Time.x)).x * 2.0 - 1.0;
  float flickerNoise = texture(textures[1], vec2(0.0, fc.g_Time.x)).x;
  vec2 grainUv = vec2((vTexCoord.x + 9.0 * fc.g_Time.y) * 0.333333, (vTexCoord.y + 3.0 * fc.g_Time.z) * 0.133333);
  float grain = texture(textures[3], grainUv).w;

  float jitter = jitterNoise * jitterNoise * jitterNoise * fc.g_Time.w;
  vec3 scene = texture(textures[0], vec2(vTexCoord.x, vTexCoord.y + jitter)).rgb;

  float luma = dot(scene, vec3(0.29891, 0.58661, 0.11448));
  float cr = dot(scene, vec3(0.5, -0.41869, -0.08131));
  float cb = dot(scene, vec3(-0.16874, -0.33126, 0.5));

  float brightness = clamp(fc.g_Range.x * (1.0 + 0.4 * flickerNoise) * luma + fc.g_Range.y, fc.g_Range.z, fc.g_Range.w);
  float newLuma = brightness * grain;
  outColor = vec4(newLuma + 1.402 * cr,
                  newLuma - 0.34414 * cb - 0.71414 * cr,
                  newLuma + 1.772 * cb, 1.0);
#endif
}
