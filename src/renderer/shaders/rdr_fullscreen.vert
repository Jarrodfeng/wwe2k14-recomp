#version 450
// A triangle covering the whole target (settings parity S10b: the DLSS motion-vector pass). No inputs, no constants.
void main() {
  vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
