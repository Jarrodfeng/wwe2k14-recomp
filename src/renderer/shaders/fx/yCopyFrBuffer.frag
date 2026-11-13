#version 450
// yCopyFrBuffer: plain copy of the frame texture.
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  outColor = texture(textures[0], vTexCoord);
}
