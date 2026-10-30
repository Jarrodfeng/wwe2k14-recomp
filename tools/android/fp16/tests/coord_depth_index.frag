#version 450
// What must stay full precision: coordinate math (also through a dependent read), an index made from a float, depth.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform U { vec4 k[4]; float scale; };
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 o;
void main() {
  vec2 c = uv * scale + 0.5;           // coordinate: full (FMul, FAdd)
  vec4 t = texture(tex, c);            // feeds the next coordinate: full
  vec4 t2 = texture(tex, t.xy * 2.0);  // VectorTimesScalar full; this sample relaxed
  int i = int(col.w * 3.0);            // index: full (FMul)
  o = t * col + k[i] * t2;             // relaxed (FMul, FMul, FAdd)
  gl_FragDepth = col.x * 0.5 + 0.25;   // depth: full (FMul, FAdd)
}
