#version 450
// Like the translator's guest register file: a Function array with constant indices. The value stored into r[0]
// becomes a coordinate (through memory): full; r[1] / r[2] are colour maths: relaxed. Plus GLSL.std.450 calls.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 o;
void main() {
  vec4 r[4];
  r[0] = vec4(uv * 2.0, 0.0, 0.0);       // full (through r[0] into the coordinate)
  r[1] = col * col;                      // relaxed
  vec4 s = texture(tex, r[0].xy);        // relaxed
  r[2] = s * r[1];                       // relaxed
  r[3] = vec4(sqrt(col.x), max(col.y, 0.5), mix(col.z, 1.0, 0.25), 1.0);  // sqrt left alone; max / mix relaxed
  o = r[2] + r[3];                       // relaxed
}
