#version 450
// Foundation test: a coloured triangle from frame geometry, moved by a constant block.
layout(set = 0, binding = 0) uniform VertexConstants { vec4 offset; } vc;
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 0) out vec3 out_color;

void main() {
  gl_Position = vec4(in_position + vc.offset.xy, vc.offset.z, 1.0);
  out_color = in_color;
}
