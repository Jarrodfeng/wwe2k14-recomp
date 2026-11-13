#version 450
// Session 101 lane 131 (MovieOurs, docs\renderer\MOVIE_OURS.md): a movie's decoded planes turned to colour. Our own shader, not a game shader.
// Planar video as the decoder leaves it: slot 0 the luma plane (Y), slot 1 the blue-difference plane (Cb), slot 2 the red-difference plane (Cr), slot 3 the alpha
// plane; each a single-channel texture read through the slot's swizzle (red, red, red, one). The conversion is DATA, not code: three rows (a, b, c, offset) applied
// to (Y, Cb, Cr), so the matrix (BT.601 limited range, the game's own coefficients) lives in the hook, and the alpha is `plane * scale + add`.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Color;       // the quad's colour (the game's Color constant), multiplied in last
  vec4 RowR;        // rgb.r = dot(RowR.xyz, (Y, Cb, Cr)) + RowR.w
  vec4 RowG;
  vec4 RowB;
  vec4 AlphaMix;    // alpha = plane_alpha * AlphaMix.x + AlphaMix.y; z: 1 = apply the stream's alpha test, 0 = skip it (plant no_alpha_test), -1 = invert it (plant alpha_test_inverted)
} fc;
// The alpha test the stream draw asks for (the renderer's system block: alpha.x the reference, alpha.y the compare bits, 7 = always), applied to the final alpha
// exactly as the effect build applies it to a game shader (GAME_RULES_DRAWS A1-A5): NaN passes only "always" and "not equal".
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(set = 1, binding = 0) uniform sampler2D plane_y;
layout(set = 1, binding = 1) uniform sampler2D plane_cb;
layout(set = 1, binding = 2) uniform sampler2D plane_cr;
layout(set = 1, binding = 3) uniform sampler2D plane_a;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec3 yuv = vec3(texture(plane_y, in_uv).x, texture(plane_cb, in_uv).x, texture(plane_cr, in_uv).x);
  vec3 rgb = vec3(dot(fc.RowR.xyz, yuv) + fc.RowR.w, dot(fc.RowG.xyz, yuv) + fc.RowG.w, dot(fc.RowB.xyz, yuv) + fc.RowB.w);
  float alpha = texture(plane_a, in_uv).x * fc.AlphaMix.x + fc.AlphaMix.y;
  out_color = vec4(rgb, alpha) * fc.Color;
  int func = int(fx_sys.alpha.y);
  if (func != 7 && fc.AlphaMix.z != 0.0) {
    float a = out_color.a;
    bool pass_test = isnan(a) ? func == 5 : ((a < fx_sys.alpha.x && (func & 1) != 0) || (a == fx_sys.alpha.x && (func & 2) != 0) || (a > fx_sys.alpha.x && (func & 4) != 0));
    if (pass_test == (fc.AlphaMix.z < 0.0)) discard;
  }
}
