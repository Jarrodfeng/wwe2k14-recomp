#version 450
// Session 101 lane 9 (our 2D pass): a sprite is texture * vertex colour; a glyph takes only the font sheet's alpha (the game's font sheets hold
// white glyphs in the alpha channel, docs\formats\ui_font.md) and the vertex colour's RGB.
// Lane 23: mode 2 is a PREMULTIPLIED sprite (blend mode 3): the texture's colour already carries its alpha, so the vertex colour's alpha fades all four
// channels (colour * alpha as well as alpha), and the blend is One / OneMinusSrcAlpha.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 mode;  // x: 0 sprite, 1 glyph, 2 premultiplied sprite
} fc;
layout(set = 1, binding = 0) uniform sampler2D source_image;
layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 t = texture(source_image, in_uv);
  if (fc.mode.x > 1.5) {
    out_color = vec4(in_color.rgb * in_color.a * t.rgb, in_color.a * t.a);
  } else if (fc.mode.x > 0.5) {
    out_color = vec4(in_color.rgb, in_color.a * t.a);
  } else {
    out_color = in_color * t;
  }
}
