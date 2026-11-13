#version 450
layout(set = 1, binding = 0) uniform sampler2D source_image;  // texture slot 0: every slot is its own binding
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  out_color = texture(source_image, in_uv);
}
