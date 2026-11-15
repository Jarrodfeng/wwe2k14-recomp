#version 450
// S102 lane L2 (docs\renderer\SHADOWS_OURS.md, "S102 L2"): the plane shadow bake's fullscreen passes, vertex stage. Our own shader, not a game shader. One triangle covering the whole target, no vertex
// buffer. The positions are in the GAME's clip space (+y up) and go through the draw's system block like every other program's, so the target's y orientation is the one the game's own plane passes use
// (clip y = +1 is the top row, texture v = 0.5 - 0.5 * clip y).
// The composite's camera (M = PlaneInv * CameraViewProj, three rows) lives in THIS block, not the pixel block: the 120 Hz frame pump interpolates only vertex-constant ranges between two game ticks, and the
// mat's own camera is interpolated, so the composite's must be too. The rows go to the pixel stage as flat varyings.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 Unused;
  vec4 RowX;   // M row 0
  vec4 RowY;   // M row 1
  vec4 RowW;   // M row 3
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(location = 0) out vec2 out_clip;
layout(location = 1) flat out vec4 out_row_x;
layout(location = 2) flat out vec4 out_row_y;
layout(location = 3) flat out vec4 out_row_w;
invariant gl_Position;
void main() {
  vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // (0,0) (2,0) (0,2)
  vec2 clip = corner * 2.0 - 1.0;                                       // (-1,-1) (3,-1) (-1,3): covers [-1,1]^2
  out_clip = clip;
  out_row_x = vc.RowX, out_row_y = vc.RowY, out_row_w = vc.RowW;
  gl_Position = vec4(vec3(clip, 0.0) * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz, 1.0);
}
