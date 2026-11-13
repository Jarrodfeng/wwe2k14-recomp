#version 450
// Session 101 lane 131 (MovieOurs, docs\renderer\MOVIE_OURS.md): the movie plane quad. Our own shader, not a game shader.
// The quad is built here from one rectangle (no vertex buffer): corners 0..3 are the stream quad's vertices in the stream's order (top-left, top-right,
// bottom-right, bottom-left), drawn as the triangles 0 1 2 and 0 2 3 (the stream's own index list). Positions are in the game's screen units; they become
// clip space the way the game's rectangle program does it: (position - half a unit) times the unit size, y flipped. The renderer's system block then maps
// the game's clip space to the target exactly as it does for every stream draw (render scale, jitter, y).
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 UnitScale;  // xy: clip units per screen unit (2 / width, 2 / height); z: the quad's depth; w: the half unit taken off every position (0.5)
  vec4 Rect;       // x0 y0 x1 y1: screen units; corner 0 is (x0, y0), corner 2 is (x1, y1)
  vec4 UvRect;     // u0 v0 u1 v1, the same corners
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
invariant gl_Position;   // review S4: as the stream programs' wrapper (UI edges sit on pixel centres)
layout(location = 0) out vec2 out_uv;

void main() {
  const int corner_of[6] = int[](0, 1, 2, 0, 2, 3);
  int corner = corner_of[gl_VertexIndex];
  bool right = corner == 1 || corner == 2;
  bool bottom = corner >= 2;
  vec2 position = vec2(right ? vc.Rect.z : vc.Rect.x, bottom ? vc.Rect.w : vc.Rect.y);
  out_uv = vec2(right ? vc.UvRect.z : vc.UvRect.x, bottom ? vc.UvRect.w : vc.UvRect.y);
  vec2 scaled = (position - vc.UnitScale.w) * vc.UnitScale.xy;
  vec4 clip = vec4(scaled.x - 1.0, 1.0 - scaled.y, vc.UnitScale.z, 1.0);
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
