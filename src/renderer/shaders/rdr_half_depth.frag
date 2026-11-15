#version 450
// Session 102 lane C4 (half-size transparents, docs\renderer\COLOUR_PASS.md): the half-size depth the tail of additive scene draws is tested against. One full-target triangle (rdr_fullscreen.vert) into a
// depth-only target of half the size; every half texel takes the NEAREST of the four full-size depths under it (the renderer's depth is reversed: larger = nearer), so a glow is hidden wherever any of
// its four pixels is covered; the composite's depth-aware upsample gives the other pixels their glow back. Our own shader.
layout(set = 1, binding = 0) uniform sampler2D full_depth;
void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy) * 2;
  const ivec2 last = textureSize(full_depth, 0) - 1;
  const float a = texelFetch(full_depth, min(p, last), 0).x;
  const float b = texelFetch(full_depth, min(p + ivec2(1, 0), last), 0).x;
  const float c = texelFetch(full_depth, min(p + ivec2(0, 1), last), 0).x;
  const float d = texelFetch(full_depth, min(p + ivec2(1, 1), last), 0).x;
  gl_FragDepth = max(max(a, b), max(c, d));
}
