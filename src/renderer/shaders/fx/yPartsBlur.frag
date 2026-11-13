#version 450
// yPartsBlur: 13-tap weighted blur (offsets and weights are constants, so the same shader serves horizontal and vertical passes).
// Alpha is the vertex colour's alpha, passed through unchanged.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 g_avSampleOffsets[13];
  vec4 g_avSampleWeights[13];
} fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
  vec3 sum = vec3(0.0);
  for (int i = 0; i < 13; i++) sum += texture(textures[0], vTexCoord + fc.g_avSampleOffsets[i].xy).rgb * fc.g_avSampleWeights[i].xyz;
  outColor = vec4(sum, vColor.a);
}
