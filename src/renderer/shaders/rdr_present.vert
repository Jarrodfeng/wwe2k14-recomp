#version 450
// The final draw: the finished picture onto the swapchain, rotated for the display's native orientation.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 rotation;  // the 2x2 turning clip-space positions
  vec4 uv_rect;   // source rectangle: offset.xy, scale.xy
} vc;
layout(location = 0) out vec2 out_uv;

void main() {
  vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  out_uv = vc.uv_rect.xy + corner * vc.uv_rect.zw;
  vec2 position = corner * 2.0 - 1.0;
  gl_Position = vec4(dot(position, vc.rotation.xy), dot(position, vc.rotation.zw), 0.0, 1.0);
}
