#version 450
// S105 A8: the MATRIX variant of the vertex-colour rectangle (rdr_uiquad_vcol.vert; docs\research\s105\A8.md). Our own shader, not a game shader.
// Placement exactly as rdr_uiquad_proj.vert (clip[k] = dot(Mat[k], (x y z 1)), the game's fUseProjMatrix = 1 rule, a quad list: triangles 0 1 2 / 0 2 3); the colour per corner as rdr_uiquad_vcol.vert.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 UnitScale;  // unused here (the matrix holds the scale)
  vec4 Pos[4];     // xyz of corner i: the game's vertex position and depth
  vec4 Col[4];     // rgba of corner i: the corner's vertex colour (0..1)
  vec4 Mat[4];     // register 29 + k as uploaded (k = 0..3)
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
invariant gl_Position;
layout(location = 0) out vec4 out_color;

void main() {
  const int list_of[6] = int[](0, 1, 2, 0, 2, 3);
  int corner = list_of[gl_VertexIndex];
  vec4 q = vec4(vc.Pos[corner].xyz, 1.0);
  out_color = vc.Col[corner];
  vec4 clip = vec4(dot(vc.Mat[0], q), dot(vc.Mat[1], q), dot(vc.Mat[2], q), dot(vc.Mat[3], q));
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
