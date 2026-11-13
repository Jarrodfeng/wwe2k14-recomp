#version 450
// yAfterImage: the colour of the source with its alpha replaced by a constant (blends the previous frame in as a trail).
layout(set = 0, binding = 1, std140) uniform FragmentConstants { vec4 g_fAlpha; } fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  outColor = vec4(texture(textures[0], vTexCoord).rgb, fc.g_fAlpha.x);
}
