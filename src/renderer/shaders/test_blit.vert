#version 450
// FLIP-EXEMPT: test shader (rplay / stencil selftest only): a full-screen blit in the target's own clip space, never a renderer pass.
// Foundation test: a full-screen triangle with no vertex input.
layout(location = 0) out vec2 out_uv;

void main() {
  out_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(out_uv * 2.0 - 1.0, 0.0, 1.0);
}
