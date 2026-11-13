#version 450
// Session 101 lane 151 (RimChainOurs): the rim light map's blur grid. The game covers the 512x256 target with a 32 x 16 grid of cells (33 x 17 points, x = -1 + column / 16,
// y = -1 + row / 8, u = column / 32, v = 1 - row / 16: exact binary fractions), and switches a point off where the coverage map (32 x 16, read at the point's own texture
// coordinate, vertex slot 16) is zero, so only the cells the rim touches are blurred. This is the same grid as a triangle list built from gl_VertexIndex (two triangles a cell,
// on the game's diagonal and with its winding), and the same switch: a point is (position * VertexConvert.xy + VertexConvert.zw) * on with w = on, z = 1.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 VertexConvert;   // xy scale, zw offset of the grid position (the game's constant for the draw, passed through as it was bound)
  vec4 Options;         // x: non-zero = every point is on (the coverage map is not read: the experiment 'unmasked', see RIM_MAP_OURS.md); 0 = the game's switch
} vc;
layout(set = 1, binding = 16) uniform sampler2D coverage;   // the game's vertex texture slot 16 (the 32 x 16 reduction)
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(location = 0) out vec2 out_uv;
invariant gl_Position;

void main() {
  const int kColumns = 32;
  // Per cell, with a = (column, row) and b = (column, row + 1) of the left edge, a0 a1 b0 / b0 a1 b1: the game's strip a0 b0 a1 b1 as Vulkan emits it (same diagonal, same winding, same first vertex).
  const ivec2 corner_of[6] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1));
  int cell = gl_VertexIndex / 6;
  ivec2 point = ivec2(cell % kColumns, cell / kColumns) + corner_of[gl_VertexIndex % 6];
  vec2 position = vec2(-1.0 + float(point.x) / 16.0, -1.0 + float(point.y) / 8.0);
  out_uv = vec2(float(point.x) / 32.0, 1.0 - float(point.y) / 16.0);
  float on = (vc.Options.x != 0.0 || textureLod(coverage, out_uv, 0.0).x != 0.0) ? 1.0 : 0.0;
  vec4 clip = vec4((position * vc.VertexConvert.xy + vc.VertexConvert.zw) * on, 1.0, on);
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
