#version 450
// Session 101 lane 151 (RimChainOurs, docs\renderer\RIM_MAP_OURS.md "P3"): the rim light map's coverage reductions, the quad. Our own shader, not a game shader.
// The game draws each reduction as one rectangle in the target's pixel units (its half pixel inside the numbers), four vertices (x, y, 1, 1, u, v), the triangles 0 1 2 and 0 2 3,
// through a program that passes the position straight through. This builds the same rectangle from two constants (no vertex buffer) and passes it on the same way: the renderer's
// system block then maps the game's clip space to the target exactly as it does for every stream draw.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 Rect;     // x0 y0 x1 y1: the stream's own corner positions (corner 0 = (x0, y0), corner 2 = (x1, y1))
  vec4 UvRect;   // u0 v0 u1 v1: the same corners' texture coordinates
  vec4 Depth;    // x: z, y: w of every corner (the stream's)
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(location = 0) out vec2 out_uv;
invariant gl_Position;

void main() {
  const int corner_of[6] = int[](0, 1, 2, 0, 2, 3);
  int corner = corner_of[gl_VertexIndex];
  bool right = corner == 1 || corner == 2;
  bool bottom = corner >= 2;
  out_uv = vec2(right ? vc.UvRect.z : vc.UvRect.x, bottom ? vc.UvRect.w : vc.UvRect.y);
  vec4 clip = vec4(right ? vc.Rect.z : vc.Rect.x, bottom ? vc.Rect.w : vc.Rect.y, vc.Depth.x, vc.Depth.y);
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
