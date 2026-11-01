#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: every shader of the frame ours): the 2D overlay sprites (the game's ySprite, guest
// pixel shader 7754C61815DA6614; the entrance's city / show logos drawn over the picture, 22 draws), through the shader
// swap (src\scene_swap.h). One texture read at the first interpolator's coordinate (fetch constant 0), then a chain of
// mode tests the guest keeps in the third component of its register 0 (called "mode" here): it starts as the sum of the
// vertex colour's four channels times c254.w; a first test (c254.x zero and that sum not) and a second (the mode zero
// and the fourth interpolator's x zero) each move the mode on (a test that holds leaves it 0, else mode + 1, or c254.x
// + 1 for the first); then the output is chosen by the tests that follow:
//   - the second test held: a "value" (the texture dotted with the vertex colour if the first test held, else the
//     coordinate's x) is compared with c254.y; red, green and blue are value * c255.x + c254.z when it is above, else
//     c254.x; the value itself is replaced by c254.w when it is above; alpha is twice that value; the output is
//     those four numbers times the third interpolator;
//   - then, if the mode has become 1: the third interpolator with its alpha times the value, as the output;
//   - then the mode is stepped down by one (0 if that is not above zero), and if it has become 1 again the output is
//     the texture times the third interpolator.
// A pixel none of these reach is written as 0. What each mode is for is unconfirmed. The texture read is plain (the
// fetch constant's LOD bias and exponent adjust are dropped). Float constants (set 1 binding 2): 254, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[2]; } fc;
#define C254 fc.c[0]
#define C255 fc.c[1]

layout(set = 3, binding = 1000) uniform texture2DArray t_sprite;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_sprite;

layout(location = 0) in vec4 in_uv;      // xy: the sprite's coordinate
layout(location = 1) in vec4 in_color;   // the vertex colour
layout(location = 2) in vec4 in_tint;    // multiplies the output of every mode
layout(location = 3) in vec4 in_flags;   // x: zero selects the first mode

// The guest's "set predicate and invert" step on the mode: 1 -> 0, 0 -> 1, else unchanged; the predicate is mode == 1.
float InvertStep(float mode, out bool held) {
  held = (mode == 1.0);
  return held ? 0.0 : ((mode == 0.0) ? 1.0 : mode);
}

void main() {
  // the mode starts as the vertex colour's channel sum times c254.w, added in the guest's order (w, x, y, z)
  float mode = in_color.w * C254.w;
  mode = fma(in_color.x, C254.w, mode);
  mode = fma(in_color.y, C254.w, mode);
  mode = fma(in_color.z, C254.w, mode);
  bool first_held = (C254.x == 0.0) && (mode != 0.0);
  mode = (first_held ? -1.0 : C254.x) + 1.0;

  vec4 tex = texture(sampler2DArray(t_sprite, s_sprite), vec3(in_uv.xy, 0.0));

  // the value: the texture dotted with the vertex colour (summed in the order w, z, x, y) if the first test held,
  // else the coordinate's x
  float value = in_uv.x;
  if (first_held) {
    value = tex.w * in_color.w;
    value = fma(tex.z, in_color.z, value);
    value = fma(tex.x, in_color.x, value);
    value = fma(tex.y, in_color.y, value);
  }

  vec4 result = vec4(0.0);
  bool second_held = (mode == 0.0) && (in_flags.x == 0.0);
  mode = (second_held ? -1.0 : mode) + 1.0;
  if (second_held) {
    float above = float(value > C254.y);
    float shade = fma(value, C255.x, C254.z);
    if (above == 0.0) shade = C254.x;
    if (above != 0.0) value = C254.w;
    float alpha = value + value;
    result = vec4(shade, shade, shade, alpha) * in_tint;
  }

  bool held;
  mode = InvertStep(mode, held);
  vec4 tint = in_tint;
  if (held) {
    tint.w = value * in_tint.w;
    result = tint;
  }

  // step the mode down by one; zero if that is not above zero
  float stepped = mode - 1.0;
  mode = (stepped <= 0.0) ? 0.0 : stepped;
  mode = InvertStep(mode, held);
  if (held) result = tex * tint;
  XeOutputColor(result);
}
