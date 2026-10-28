#version 450
// Session 72 (diagnostic, --wwe2k14_post_swap_probe=1): in place of the entrance combine (4E2CD8031D93DE3E), paints
// what the pass receives so a screenshot gives exact numbers (tools\post\read_probe.py --entrance). The left half shows
// the scene, the depth, the far blur, the near blur, the bloom (fetch constant 7) and the glare (8) side by side; the
// right half holds values as bits - one row per 32-bit value, 32 black / white cells (bit 31 first): rows 0-51 the 13
// float constants' components (packed order c20 c21 c27 c29 c30 c31 c32 c250 c251 c252 c253 c254 c255, x y z w each),
// rows 52-57 the six textures' sizes (width << 16 | height: fetch constants 0 2 3 6 7 8), rows 58-61 the interpolator's
// x y z w at this row (as float bits), rows 62-67 the bound samplers' filtering (how many of 64 points sample
// differently 0.3 texel off a texel centre: 0 = point sampling).

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[13]; } fc;
layout(set = 3, binding = 1000) uniform texture2D t_scene;
layout(set = 3, binding = 1002) uniform texture2D t_blur_a;
layout(set = 3, binding = 1003) uniform texture2D t_depth;
layout(set = 3, binding = 1006) uniform texture2D t_blur_b;
layout(set = 3, binding = 1007) uniform texture2D t_bloom;
layout(set = 3, binding = 1008) uniform texture2D t_glare;
layout(set = 3, binding = 1200) uniform sampler s_scene;
layout(set = 3, binding = 1202) uniform sampler s_blur_a;
layout(set = 3, binding = 1203) uniform sampler s_depth;
layout(set = 3, binding = 1206) uniform sampler s_blur_b;
layout(set = 3, binding = 1207) uniform sampler s_bloom;
layout(set = 3, binding = 1208) uniform sampler s_glare;

layout(location = 0) in vec4 in_texcoord;
layout(location = 0) out vec4 out_color;

const int kRows = 68;

uint SizeWord(ivec2 s) { return (uint(s.x) << 16) | uint(s.y); }

void main() {
  vec2 uv = in_texcoord.xy;
  if (uv.x >= 0.5) {
    int row = int(uv.y * float(kRows));
    int bit = 31 - int((uv.x - 0.5) * 2.0 * 32.0);
    uint word = 0u;
    if (row < 52) {
      word = floatBitsToUint(fc.c[row / 4][row % 4]);
    } else if (row == 52) {
      word = SizeWord(textureSize(sampler2D(t_scene, s_scene), 0));
    } else if (row == 53) {
      word = SizeWord(textureSize(sampler2D(t_blur_a, s_blur_a), 0));
    } else if (row == 54) {
      word = SizeWord(textureSize(sampler2D(t_depth, s_depth), 0));
    } else if (row == 55) {
      word = SizeWord(textureSize(sampler2D(t_blur_b, s_blur_b), 0));
    } else if (row == 56) {
      word = SizeWord(textureSize(sampler2D(t_bloom, s_bloom), 0));
    } else if (row == 57) {
      word = SizeWord(textureSize(sampler2D(t_glare, s_glare), 0));
    } else if (row < 62) {
      word = floatBitsToUint(in_texcoord[row - 58]);
    } else if (row < 68) {
      // The bound sampler's filtering: of 64 points spread over the texture, how many sample differently 0.3 texel off
      // a texel centre than at the centre (0 = point sampling, or a flat texture).
      word = 0u;
      for (int i = 0; i < 64; ++i) {
        vec2 p = (vec2(float(i % 8), float(i / 8)) + 0.5) / 8.0;
        vec3 a, b;
        if (row == 62) {
          ivec2 s = textureSize(sampler2D(t_scene, s_scene), 0);
          vec2 c = (floor(p * vec2(s)) + 0.5) / vec2(s);
          a = textureLod(sampler2D(t_scene, s_scene), c, 0.0).rgb;
          b = textureLod(sampler2D(t_scene, s_scene), c + 0.3 / vec2(s), 0.0).rgb;
        } else if (row == 63) {
          ivec2 s = textureSize(sampler2D(t_blur_a, s_blur_a), 0);
          vec2 c = (floor(p * vec2(s)) + 0.5) / vec2(s);
          a = textureLod(sampler2D(t_blur_a, s_blur_a), c, 0.0).rgb;
          b = textureLod(sampler2D(t_blur_a, s_blur_a), c + 0.3 / vec2(s), 0.0).rgb;
        } else if (row == 64) {
          ivec2 s = textureSize(sampler2D(t_depth, s_depth), 0);
          vec2 c = (floor(p * vec2(s)) + 0.5) / vec2(s);
          a = textureLod(sampler2D(t_depth, s_depth), c, 0.0).rgb;
          b = textureLod(sampler2D(t_depth, s_depth), c + 0.3 / vec2(s), 0.0).rgb;
        } else if (row == 65) {
          ivec2 s = textureSize(sampler2D(t_blur_b, s_blur_b), 0);
          vec2 c = (floor(p * vec2(s)) + 0.5) / vec2(s);
          a = textureLod(sampler2D(t_blur_b, s_blur_b), c, 0.0).rgb;
          b = textureLod(sampler2D(t_blur_b, s_blur_b), c + 0.3 / vec2(s), 0.0).rgb;
        } else if (row == 66) {
          ivec2 s = textureSize(sampler2D(t_bloom, s_bloom), 0);
          vec2 c = (floor(p * vec2(s)) + 0.5) / vec2(s);
          a = textureLod(sampler2D(t_bloom, s_bloom), c, 0.0).rgb;
          b = textureLod(sampler2D(t_bloom, s_bloom), c + 0.3 / vec2(s), 0.0).rgb;
        } else {
          ivec2 s = textureSize(sampler2D(t_glare, s_glare), 0);
          vec2 c = (floor(p * vec2(s)) + 0.5) / vec2(s);
          a = textureLod(sampler2D(t_glare, s_glare), c, 0.0).rgb;
          b = textureLod(sampler2D(t_glare, s_glare), c + 0.3 / vec2(s), 0.0).rgb;
        }
        if (any(notEqual(a, b))) ++word;
      }
    }
    float on = ((word >> uint(clamp(bit, 0, 31))) & 1u) != 0u ? 1.0 : 0.0;
    out_color = vec4(on, on, on, 1.0);
    return;
  }
  // Left half: six squeezed views of the inputs (bloom and glare scaled up 4x to be seen).
  int column = int(uv.x * 12.0);
  vec2 v = vec2(fract(uv.x * 12.0), uv.y);
  vec3 rgb;
  if (column < 1) {
    rgb = textureLod(sampler2D(t_scene, s_scene), v, 0.0).rgb;
  } else if (column < 2) {
    rgb = vec3(textureLod(sampler2D(t_depth, s_depth), v, 0.0).x * 100.0);
  } else if (column < 3) {
    rgb = textureLod(sampler2D(t_blur_a, s_blur_a), v, 0.0).rgb;
  } else if (column < 4) {
    rgb = textureLod(sampler2D(t_blur_b, s_blur_b), v, 0.0).rgb;
  } else if (column < 5) {
    rgb = textureLod(sampler2D(t_bloom, s_bloom), v, 0.0).rgb * 4.0;
  } else {
    rgb = textureLod(sampler2D(t_glare, s_glare), v, 0.0).rgb * 4.0;
  }
  out_color = vec4(rgb, 1.0);
}
