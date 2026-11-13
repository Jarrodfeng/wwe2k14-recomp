#version 450
// Session 101 lane 148 (UiQuads, docs\renderer\UI_QUADS_OURS.md): the UI sprite's pixel step. Our own shader, not a game shader.
// T_Sprite = the picture in slot 0 times the quad's colour (the game's yAudience colour-alpha pixel program with no light map: texture * colour, alpha = texture alpha * colour alpha).
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Color;     // the quad's colour (the game's Color constant)
  vec4 AlphaMix;  // alpha = alpha * x + y (1, 0 = as drawn; plant no_blend); z: 1 = apply the stream's alpha test, 0 = skip it
} fc;
// The alpha test the stream draw asks for (the renderer's system block: alpha.x the reference, alpha.y the compare bits, 7 = always), applied to the final alpha as the
// effect build applies it to a game shader: NaN passes only "always" and "not equal".
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(set = 1, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 texel = texture(picture, in_uv);
  out_color = vec4(texel.rgb * fc.Color.rgb, texel.a * fc.Color.a * fc.AlphaMix.x + fc.AlphaMix.y);
  int func = int(fx_sys.alpha.y);
  if (func != 7 && fc.AlphaMix.z != 0.0) {
    float a = out_color.a;
    bool pass_test = isnan(a) ? func == 5 : ((a < fx_sys.alpha.x && (func & 1) != 0) || (a == fx_sys.alpha.x && (func & 2) != 0) || (a > fx_sys.alpha.x && (func & 4) != 0));
    if (!pass_test) discard;
  }
}
