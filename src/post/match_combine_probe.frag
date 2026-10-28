#version 450
// Session 72 (diagnostic, --wwe2k14_post_swap_probe): in place of the match combine (1233F68C15FDF948), paints what
// the pass receives so a screenshot gives exact numbers (tools\post\read_probe.py). The left half shows the scene, the
// depth, the far and the near input side by side (quarter width each, squeezed); the right half holds values as bits -
// one row per 32-bit value, 32 black / white cells (bit 31 first), so the presenter's gamma ramp cannot bend them:
// rows 0-43 the 11 float constants' components (packed order c20 c21 c27 c29 c30 c250 c251 c252 c253 c254 c255, x y z
// w each), rows 44-47 the four textures' sizes (width << 16 | height: scene, depth, far, near), rows 48-55 the depth
// at 8 points down the screen centre, row 56 the interpolator's x at this row (as float bits), row 57 its y.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[11]; } fc;
layout(set = 3, binding = 1000) uniform texture2D t_scene;
layout(set = 3, binding = 1002) uniform texture2D t_far;
layout(set = 3, binding = 1003) uniform texture2D t_depth;
layout(set = 3, binding = 1006) uniform texture2D t_near;
layout(set = 3, binding = 1200) uniform sampler s_scene;
layout(set = 3, binding = 1202) uniform sampler s_far;
layout(set = 3, binding = 1203) uniform sampler s_depth;
layout(set = 3, binding = 1206) uniform sampler s_near;

layout(location = 0) in vec4 in_texcoord;
layout(location = 0) out vec4 out_color;

const int kRows = 58;

uint SizeWord(ivec2 s) { return (uint(s.x) << 16) | uint(s.y); }

void main() {
  vec2 uv = in_texcoord.xy;
  if (uv.x >= 0.5) {
    int row = int(uv.y * float(kRows));
    int bit = 31 - int((uv.x - 0.5) * 2.0 * 32.0);
    uint word = 0u;
    if (row < 44) {
      word = floatBitsToUint(fc.c[row / 4][row % 4]);
    } else if (row == 44) {
      word = SizeWord(textureSize(sampler2D(t_scene, s_scene), 0));
    } else if (row == 45) {
      word = SizeWord(textureSize(sampler2D(t_depth, s_depth), 0));
    } else if (row == 46) {
      word = SizeWord(textureSize(sampler2D(t_far, s_far), 0));
    } else if (row == 47) {
      word = SizeWord(textureSize(sampler2D(t_near, s_near), 0));
    } else if (row < 56) {
      word = floatBitsToUint(textureLod(sampler2D(t_depth, s_depth), vec2(0.5, (float(row - 48) + 0.5) / 8.0), 0.0).x);
    } else if (row == 56) {
      word = floatBitsToUint(in_texcoord.x);
    } else if (row == 57) {
      word = floatBitsToUint(in_texcoord.y);
    }
    float on = ((word >> uint(clamp(bit, 0, 31))) & 1u) != 0u ? 1.0 : 0.0;
    out_color = vec4(on, on, on, 1.0);
    return;
  }
  // Left half: four squeezed views of the inputs.
  int column = int(uv.x * 8.0);
  vec2 v = vec2(fract(uv.x * 8.0), uv.y);
  if (column < 1) {
    out_color = vec4(textureLod(sampler2D(t_scene, s_scene), v, 0.0).rgb, 1.0);
  } else if (column < 2) {
    float d = textureLod(sampler2D(t_depth, s_depth), v, 0.0).x;
    out_color = vec4(d, d, d, 1.0);
  } else if (column < 3) {
    out_color = vec4(textureLod(sampler2D(t_far, s_far), v, 0.0).rgb, 1.0);
  } else {
    out_color = vec4(textureLod(sampler2D(t_near, s_near), v, 0.0).rgb, 1.0);
  }
}
