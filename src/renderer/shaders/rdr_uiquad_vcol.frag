#version 450
// S105 A8: the vertex-colour rectangle's pixel stage (T_RectColor; docs\research\s105\A8.md). Our own shader, not a game shader.
// The game's yRect vertex_color variant: the vertex stage makes inColor * Color and the pixel stage passes it straight through. Here the interpolated corner colour comes in and Color (c1) multiplies it.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Color;     // the object's Color constant (c1)
  vec4 AlphaMix;  // alpha = alpha * x + y (1, 0 = as drawn); z: 1 = apply the stream's alpha test, 0 = skip it
} fc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(location = 0) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 c = in_color * fc.Color;
  out_color = vec4(c.rgb, c.a * fc.AlphaMix.x + fc.AlphaMix.y);
  int func = int(fx_sys.alpha.y);
  if (func != 7 && fc.AlphaMix.z != 0.0) {
    float a = out_color.a;
    bool pass_test = isnan(a) ? func == 5 : ((a < fx_sys.alpha.x && (func & 1) != 0) || (a == fx_sys.alpha.x && (func & 2) != 0) || (a > fx_sys.alpha.x && (func & 4) != 0));
    if (!pass_test) discard;
  }
}
