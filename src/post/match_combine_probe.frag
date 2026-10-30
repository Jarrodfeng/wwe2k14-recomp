#version 450
// Session 72 (diagnostic, --wwe2k14_post_swap_probe): in place of the match combine (1233F68C15FDF948), paints what
// the pass receives so a screenshot gives exact numbers (tools\post\read_probe.py). The left half shows the scene, the
// depth, the far and the near input side by side (quarter width each, squeezed); the right half holds values as bits -
// one row per 32-bit value, 32 black / white cells (bit 31 first), so the presenter's gamma ramp cannot bend them:
// rows 0-43 the 11 float constants' components (packed order c20 c21 c27 c29 c30 c250 c251 c252 c253 c254 c255, x y z
// w each), rows 44-47 the four textures' sizes (width << 16 | height: scene, depth, far, near), rows 48-55 the depth
// at 8 points down the screen centre, row 56 the interpolator's x at this row (as float bits), row 57 its y.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[11]; } fc;
layout(set = 3, binding = 1000) uniform texture2DArray t_scene;
layout(set = 3, binding = 1002) uniform texture2DArray t_far;
layout(set = 3, binding = 1003) uniform texture2DArray t_depth;
layout(set = 3, binding = 1006) uniform texture2DArray t_near;
layout(set = 3, binding = 1200) uniform sampler s_scene;
layout(set = 3, binding = 1202) uniform sampler s_far;
layout(set = 3, binding = 1203) uniform sampler s_depth;
layout(set = 3, binding = 1206) uniform sampler s_near;

// Session 73: the texture cache binds every 2D texture as a 2D *array* view (layer 0), as the translated shaders
// declare them; reading one through a plain 2D type is undefined (validation VUID-vkCmdDraw-viewType-07752: worked
// on NVIDIA and the Adreno 830 by luck). These read layer 0 of the array.
vec4 Lod(texture2DArray t, sampler s, vec2 uv) { return textureLod(sampler2DArray(t, s), vec3(uv, 0.0), 0.0); }
ivec2 Size(texture2DArray t, sampler s) { return textureSize(sampler2DArray(t, s), 0).xy; }
vec4 Fetch(texture2DArray t, sampler s, ivec2 p) { return texelFetch(sampler2DArray(t, s), ivec3(p, 0), 0); }

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
      word = SizeWord(Size(t_scene, s_scene));
    } else if (row == 45) {
      word = SizeWord(Size(t_depth, s_depth));
    } else if (row == 46) {
      word = SizeWord(Size(t_far, s_far));
    } else if (row == 47) {
      word = SizeWord(Size(t_near, s_near));
    } else if (row < 56) {
      word = floatBitsToUint(Lod(t_depth, s_depth, vec2(0.5, (float(row - 48) + 0.5) / 8.0)).x);
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
    out_color = vec4(Lod(t_scene, s_scene, v).rgb, 1.0);
  } else if (column < 2) {
    float d = Lod(t_depth, s_depth, v).x;
    out_color = vec4(d, d, d, 1.0);
  } else if (column < 3) {
    out_color = vec4(Lod(t_far, s_far, v).rgb, 1.0);
  } else {
    out_color = vec4(Lod(t_near, s_near, v).rgb, 1.0);
  }
}
