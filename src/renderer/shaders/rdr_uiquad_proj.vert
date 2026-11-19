#version 450
// Session 103 lane P3c (docs\renderer\P3B_MENU_LEAVES.md section 5): the MATRIX variant of the UI's 2D quad. Our own shader, not a game shader.
// rdr_uiquad.vert places a quad in the game's pixel units (fUseProjMatrix = 0). The menu's text, M sprites and rects are placed by a per-object matrix instead (fUseProjMatrix = 1): the game's
// vertex program does  clip[k] = dot(matProj[k], (x y z 1))  and nothing else (no half unit, no PosScale, no y flip: the matrix holds them) [C: ySprite.vert spritePosition / yRect.vert main;
// sub_82BC2A30 uploads the object's matrix TRANSPOSED as c29..c32, so register 29 + k is row k of the game's clip transform here]. The renderer's system block then maps the game's clip space to the target
// as it does for every stream draw (render scale, jitter, y), exactly as in rdr_uiquad.vert.
// The four corners are the stream draw's vertices IN THE DRAW'S ORDER (no perimeter reordering): a strip (UnitScale.z = 1) is the stream's own two triangles 0 1 2 and 2 1 3 (a perspective-placed quad
// interpolates its uv per triangle, so the diagonal is the stream's), a quad list (UnitScale.z = 0) the triangles 0 1 2 and 0 2 3.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 UnitScale;  // z: 1 = the corners are a triangle strip (0 1 2 / 2 1 3), 0 = a quad list in perimeter order (0 1 2 / 0 2 3); x, y, w unused
  vec4 Pos[4];     // xyz of corner i: the game's vertex position and depth
  vec4 Uv[4];      // xy of corner i: the texture coordinate (unused by the flat program)
  vec4 Mat[4];     // register 29 + k as uploaded (k = 0..3)
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
invariant gl_Position;
layout(location = 0) out vec2 out_uv;

void main() {
  const int list_of[6] = int[](0, 1, 2, 0, 2, 3);
  const int strip_of[6] = int[](0, 1, 2, 2, 1, 3);
  int corner = vc.UnitScale.z != 0.0 ? strip_of[gl_VertexIndex] : list_of[gl_VertexIndex];
  vec4 q = vec4(vc.Pos[corner].xyz, 1.0);
  out_uv = vc.Uv[corner].xy;
  vec4 clip = vec4(dot(vc.Mat[0], q), dot(vc.Mat[1], q), dot(vc.Mat[2], q), dot(vc.Mat[3], q));
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
