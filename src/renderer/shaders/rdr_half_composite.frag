#version 450
// Session 102 lane C4 (half-size transparents): adds the half-size picture of the additive tail back onto the full-size scene colour. Each full-size pixel takes the four half texels around it with bilinear
// weights, each divided by how far its depth is from this pixel's own depth (the depth-aware upsample against the FULL-size depth): a half texel that sat in front of or behind this pixel's surface
// contributes little. The output is added (ONE, ONE) to the target by the pipeline's blend. push.x = 1 is the PLANTED FAULT (the plain bilinear upsample, no depth test: TEST ONLY, rplay --half-plant 1).
// Our own shader.
layout(push_constant) uniform Push { vec4 knobs; } push;
layout(set = 1, binding = 0) uniform sampler2D half_colour;
layout(set = 1, binding = 1) uniform sampler2D half_depth;
layout(set = 1, binding = 2) uniform sampler2D full_depth;
layout(location = 0) out vec4 out_colour;
void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy);
  const ivec2 hs = textureSize(half_colour, 0);
  const vec2 hp = (vec2(p) + 0.5) * 0.5 - 0.5;
  const ivec2 base = ivec2(floor(hp));
  const vec2 f = hp - vec2(base);
  // Most of the picture has no glow: when the four half texels around this pixel are all empty, leave without touching the target (a discard skips the blend's read and write).
  vec4 any_light = vec4(0.0);
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) any_light += texelFetch(half_colour, clamp(base + ivec2(i, j), ivec2(0), hs - 1), 0);
  if (any_light == vec4(0.0)) discard;
  const float d = texelFetch(full_depth, p, 0).x;
  vec4 sum = vec4(0.0);
  float wsum = 0.0;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const ivec2 q = clamp(base + ivec2(i, j), ivec2(0), hs - 1);
      const float bw = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
      const float hd = texelFetch(half_depth, q, 0).x;
      const float rel = abs(d - hd) / max(max(d, hd), 1e-5);
      const float w = push.knobs.x > 0.5 ? bw : bw / (rel * 24.0 + 0.02);
      sum += texelFetch(half_colour, q, 0) * w;
      wsum += w;
    }
  }
  out_colour = sum / max(wsum, 1e-6);
}
