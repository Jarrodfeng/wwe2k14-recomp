#version 450
layout(set = 0, binding = 1) uniform FragmentConstants { vec4 tint; } fc;
layout(location = 0) in vec3 in_color;
layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(in_color * fc.tint.rgb, 1.0);
}
