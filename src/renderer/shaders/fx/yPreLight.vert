#version 450
#extension GL_EXT_multiview : enable   // (unconditional on purpose: the wrapper puts its `invariant gl_Position` after the LAST #extension line, so a conditional one would drop it from every other variant; enable adds no capability until gl_ViewIndex is used)
// Pre-computed light cube map, vertex stage. See docs\renderer\effects\yPreLight.md.
// Draws a full-screen quad per cube face; the direction the pixel stage lights is passed straight through.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ScreenScale;      // xy scale, zw offset of the quad into the target
} vc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inDirection;
layout(location = 0) out vec4 outDirection;

void main() {
#ifdef FX_LAYERED
  vec2 p = inPosition.xy * vc.ScreenScale.xy + vc.ScreenScale.zw;
  // S102 C1 (docs/renderer/PRELIGHT_DIRECT.md section 7): ONE render pass over the cube's six 64x64 layers (multiview, view mask 0x3F). View v draws tile v of the 384x64 strip onto its whole
  // 64x64 face: the strip's clip x of tile v, [-1 + v / 3, -1 + (v + 1) / 3], becomes [-1, 1] (x * 6 + 5 - 2 v); the other tiles fall outside the clip volume. The mesh is the whole strip's.
  float tile = float(gl_ViewIndex);
#ifdef FX_LAYERED_SWAP
  tile = 5.0 - tile;   // the planted twin (c1_check: face order swapped): view v draws tile 5 - v
#endif
  p.x = p.x * 6.0 + (5.0 - 2.0 * tile);
  gl_Position = vec4(p, inPosition.z, 1.0);
#else
  gl_Position = vec4(inPosition.xy * vc.ScreenScale.xy + vc.ScreenScale.zw, inPosition.z, 1.0);   // (the original line, byte for byte: yPreLight_base's SPIR-V is the tip's)
#endif
  outDirection = vec4(inDirection, 0.0);
}
