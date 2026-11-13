#version 450
// yScaleDownFB: 4-tap box average of the frame into a smaller target. Alpha is written as 0.
layout(set = 0, binding = 1, std140) uniform FragmentConstants { vec4 sampleOffsets[4]; } fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  vec3 sum = vec3(0.0);
  for (int i = 0; i < 4; i++) sum += texture(textures[0], vTexCoord + fc.sampleOffsets[i].xy).rgb;
  outColor = vec4(sum * 0.25, 0.0);
}
