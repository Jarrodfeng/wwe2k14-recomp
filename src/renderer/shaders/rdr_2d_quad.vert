#version 450
// Session 101 lane 9 (our 2D pass, src\renderer_scene\pass2d.*): one vertex of a sprite or glyph quad. Positions are in display pixels, top-left
// origin; the pass pushes 2 / width and 2 / height. Our own shader (not a game shader).
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 screen;  // x: 2 / target width, y: 2 / target height
} vc;
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
  out_uv = in_uv;
  out_color = in_color;
  gl_Position = vec4(in_position * vc.screen.xy - 1.0, 0.0, 1.0);
}
