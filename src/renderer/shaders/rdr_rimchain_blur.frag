#version 450
// Session 101 lane 151 (RimChainOurs): the rim light map's blur: the weighted sum of 13 reads of the rim map (offsets and weights are the game's own constants for the draw,
// passed through as they were bound; every channel carries its own weight).
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 SampleOffsets[13];
  vec4 SampleWeights[13];
} fc;
layout(set = 1, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 sum = vec4(0.0);
  for (int i = 0; i < 13; ++i) sum += texture(source, in_uv + fc.SampleOffsets[i].xy) * fc.SampleWeights[i];
  out_color = sum;
}
