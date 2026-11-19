#version 450
// Session 103 lane I3 (the 2D layer's glyph rows, docs\renderer\REMNANT_MAP.md section 12): the text glyph's pixel step. Our own shader, not a game shader.
// = the game's ySprite channel_select vertex stage (the selector constant c4 -> a per-channel mask and the mask-mode flag) followed by its channel_mask pixel stage (src\renderer\shaders\fx\ySprite.vert / .frag, the
// same arithmetic), folded into one pixel step because a glyph row carries the RAW selector. The vertex placement is rdr_uiquad.vert (the glyph is a quad like any other).
// Selector: a channel whose value is in (0, 0.5] is an alpha-mask channel (sent on as 1, and it raises the mask flag); any other value is the channel's weight. No weight at all: the picture times the colour.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Color;     // the quad's colour (the game's Color constant, c1)
  vec4 AlphaMix;  // alpha = alpha * x + y (1, 0 = as drawn); z: 1 = apply the stream's alpha test, 0 = skip it
  vec4 Selector;  // the game's XfmChannelSelector (c4): which texture channel is the glyph's coverage
} fc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(set = 1, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 texel = texture(picture, in_uv);
  bvec4 is_mask = bvec4(fc.Selector.x > 0.0 && fc.Selector.x <= 0.5, fc.Selector.y > 0.0 && fc.Selector.y <= 0.5, fc.Selector.z > 0.0 && fc.Selector.z <= 0.5, fc.Selector.w > 0.0 && fc.Selector.w <= 0.5);
  vec4 select = vec4(is_mask.x ? 1.0 : fc.Selector.x, is_mask.y ? 1.0 : fc.Selector.y, is_mask.z ? 1.0 : fc.Selector.z, is_mask.w ? 1.0 : fc.Selector.w);
  bool mask_mode = any(is_mask);
  float select_sum = select.x + select.y + select.z + select.w;
  vec4 color;
  if (select_sum != 0.0) {
    float coverage = dot(texel, select);
    if (!mask_mode) {
      // distance-field style channel: above one half it becomes solid and the rim brightens
      bool solid = coverage > 0.5;
      float rim = solid ? 2.0 * coverage - 1.0 : 0.0;
      float edge = solid ? 1.0 : coverage;
      color = vec4(rim, rim, rim, edge + edge) * fc.Color;
    } else {
      // the channel is plain coverage: it becomes the alpha of a flat colour
      color = vec4(fc.Color.rgb, coverage * fc.Color.a);
    }
  } else {
    color = texel * fc.Color;
  }
  out_color = vec4(color.rgb, color.a * fc.AlphaMix.x + fc.AlphaMix.y);
  int func = int(fx_sys.alpha.y);
  if (func != 7 && fc.AlphaMix.z != 0.0) {
    float a = out_color.a;
    bool pass_test = isnan(a) ? func == 5 : ((a < fx_sys.alpha.x && (func & 1) != 0) || (a == fx_sys.alpha.x && (func & 2) != 0) || (a > fx_sys.alpha.x && (func & 4) != 0));
    if (!pass_test) discard;
  }
}
