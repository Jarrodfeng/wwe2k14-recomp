#version 450
// Session 101 lane 148 (UiQuads, docs\renderer\UI_QUADS_OURS.md): the UI's 2D quad (T_Sprite / T_Rect). Our own shader, not a game shader.
// The quad is built here from four corners given as constants (no vertex buffer): corners 0..3 are the stream quad's vertices in the stream's order, drawn as the
// triangles 0 1 2 and 0 2 3 (the stream's own index list). The corners are any four points (a rotated sprite is a quad, not a rectangle). Positions are in the
// game's screen units; they become clip space the way the game's rectangle program does it (fUseProjMatrix = 0): (position - half a unit) times the unit size,
// y flipped. The renderer's system block then maps the game's clip space to the target as it does for every stream draw (render scale, jitter, y).
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 UnitScale;  // xy: clip units per screen unit (2 / width, 2 / height; the game's PosScale); w: the half unit the game takes off every position (0.5)
  vec4 Pos[4];     // xyz of corner i: screen units and depth
  vec4 Uv[4];      // xy of corner i: the texture coordinate (unused by the flat program)
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
invariant gl_Position;   // review S4: as the stream programs' wrapper (UI edges sit on pixel centres)
layout(location = 0) out vec2 out_uv;

void main() {
  const int corner_of[6] = int[](0, 1, 2, 0, 2, 3);
  int corner = corner_of[gl_VertexIndex];
  vec3 position = vc.Pos[corner].xyz;
  out_uv = vc.Uv[corner].xy;
  vec2 scaled = (position.xy - vc.UnitScale.w) * vc.UnitScale.xy;
  vec4 clip = vec4(scaled.x - 1.0, 1.0 - scaled.y, position.z, 1.0);
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
