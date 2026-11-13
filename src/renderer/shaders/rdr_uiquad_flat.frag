#version 450
// Session 101 lane 148 (UiQuads, docs\renderer\UI_QUADS_OURS.md): the UI rectangle's pixel step (T_Rect: fades, boxes, colour bars). Our own shader, not a game shader.
// The game's rectangle pixel program passes the colour straight through: out = Color.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Color;     // the rectangle's colour (the game's Color constant)
  vec4 AlphaMix;  // alpha = alpha * x + y (1, 0 = as drawn; plant fade_alpha_ignored); z: 1 = apply the stream's alpha test, 0 = skip it
} fc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(fc.Color.rgb, fc.Color.a * fc.AlphaMix.x + fc.AlphaMix.y);
  int func = int(fx_sys.alpha.y);
  if (func != 7 && fc.AlphaMix.z != 0.0) {
    float a = out_color.a;
    bool pass_test = isnan(a) ? func == 5 : ((a < fx_sys.alpha.x && (func & 1) != 0) || (a == fx_sys.alpha.x && (func & 2) != 0) || (a > fx_sys.alpha.x && (func & 4) != 0));
    if (!pass_test) discard;
  }
}
