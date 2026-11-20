#version 450
// S105 A8: the vertex-colour rectangle's vertex stage (T_RectColor, sub_82BC3F30; docs\research\s105\A8.md). Our own shader, not a game shader.
// rdr_uiquad.vert with a colour per corner: Col[i] is corner i's RGBA (the game's vertex_color variant of yRect: out = inColor * Color, yRect.vert FX_VERTEX_COLOR), carried to the pixel
// stage interpolated across the two triangles 0 1 2 / 0 2 3 (the quad list's own). Placement as the pixel-unit rows: (position - half a unit) * UnitScale, y flipped.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 UnitScale;  // xy: clip units per screen unit (2 / width, 2 / height; the game's PosScale); w: the half unit the game takes off every position (0.5)
  vec4 Pos[4];     // xyz of corner i: screen units and depth
  vec4 Col[4];     // rgba of corner i: the corner's vertex colour (0..1)
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
invariant gl_Position;
layout(location = 0) out vec4 out_color;

void main() {
  const int corner_of[6] = int[](0, 1, 2, 0, 2, 3);
  int corner = corner_of[gl_VertexIndex];
  vec3 position = vc.Pos[corner].xyz;
  out_color = vc.Col[corner];
  vec2 scaled = (position.xy - vc.UnitScale.w) * vc.UnitScale.xy;
  vec4 clip = vec4(scaled.x - 1.0, 1.0 - scaled.y, position.z, 1.0);
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
