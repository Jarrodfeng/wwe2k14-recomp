#version 450
// Session 101 lane 151 (RimChainOurs): one coverage reduction of the rim light map: white where any of four reads of the source is non-zero, else black (a mask grown by the
// distance between the reads, shrunk to the target's size). The four offsets are the game's own constants for the draw (SampleOffsets, passed through as they were bound).
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 SampleOffsets[2];   // [0].xy [0].zw [1].xy [1].zw: the four reads' offsets in texture coordinates
} fc;
// The alpha test the stream draw asks for (the renderer's system block: alpha.x the reference, alpha.y the compare bits, 7 = always), applied to the final alpha exactly as the effect build
// applies it to a game shader (GAME_RULES_DRAWS A1-A5). The game asks for >= 0.502 on these draws and writes alpha 1, so every texel passes; the test is kept so the draw means what the stream's does.
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(set = 1, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 taps = vec4(texture(source, in_uv + fc.SampleOffsets[0].xy).x, texture(source, in_uv + fc.SampleOffsets[0].zw).x,
                   texture(source, in_uv + fc.SampleOffsets[1].xy).x, texture(source, in_uv + fc.SampleOffsets[1].zw).x);
  out_color = vec4(vec3(any(notEqual(taps, vec4(0.0))) ? 1.0 : 0.0), 1.0);
  int func = int(fx_sys.alpha.y);
  if (func != 7) {
    float a = out_color.a;
    bool pass_test = isnan(a) ? func == 5 : ((a < fx_sys.alpha.x && (func & 1) != 0) || (a == fx_sys.alpha.x && (func & 2) != 0) || (a > fx_sys.alpha.x && (func & 4) != 0));
    if (!pass_test) discard;
  }
}
