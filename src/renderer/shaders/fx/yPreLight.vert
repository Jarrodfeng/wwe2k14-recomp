#version 450
// Pre-computed light cube map, vertex stage. See docs\renderer\effects\yPreLight.md.
// Draws a full-screen quad per cube face; the direction the pixel stage lights is passed straight through.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ScreenScale;      // xy scale, zw offset of the quad into the target
} vc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inDirection;
layout(location = 0) out vec4 outDirection;

void main() {
  gl_Position = vec4(inPosition.xy * vc.ScreenScale.xy + vc.ScreenScale.zw, inPosition.z, 1.0);
  outDirection = vec4(inDirection, 0.0);
}
