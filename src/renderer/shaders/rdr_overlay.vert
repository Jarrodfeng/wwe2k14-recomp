#version 450
// Settings parity S11: the app's overlay (Dear ImGui dialogs) over the picture. Positions are in the overlay's coordinate space (the window,
// top-left origin); the present's constant block gives 2 / width and 2 / height (sgsr.yz) and the turn for the display's native orientation.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 rotation;       // the 2x2 turning clip-space positions
  vec4 uv_rect;        // (the present's)
  vec4 viewport_info;  // (the present's SGSR)
  vec4 sgsr;           // y: 2 / overlay width, z: 2 / overlay height
} vc;
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
  out_uv = in_uv;
  out_color = in_color;
  vec2 position = vec2(in_position.x * vc.sgsr.y - 1.0, in_position.y * vc.sgsr.z - 1.0);
  gl_Position = vec4(dot(position, vc.rotation.xy), dot(position, vc.rotation.zw), 0.0, 1.0);
}
