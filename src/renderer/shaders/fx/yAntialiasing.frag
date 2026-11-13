#version 450
// yAntialiasing: edge-aware blur. A Sobel filter over the 3x3 neighbourhood finds edges; edge pixels become the 3x3 average,
// others keep their colour. Options: FX_FIXED_THRESHOLD (threshold 0.64 built in, otherwise g_AntiThreshold).
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 g_f2SampleOffsets;
#ifndef FX_FIXED_THRESHOLD
  vec4 g_AntiThreshold;
#endif
} fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  vec2 o = fc.g_f2SampleOffsets.xy;
  vec4 nw = texture(textures[0], vTexCoord + vec2(-o.x, -o.y));
  vec4 n  = texture(textures[0], vTexCoord + vec2(0.0, -o.y));
  vec4 ne = texture(textures[0], vTexCoord + vec2(o.x, -o.y));
  vec4 w  = texture(textures[0], vTexCoord + vec2(-o.x, 0.0));
  vec4 c  = texture(textures[0], vTexCoord);
  vec4 e  = texture(textures[0], vTexCoord + vec2(o.x, 0.0));
  vec4 sw = texture(textures[0], vTexCoord + vec2(-o.x, o.y));
  vec4 s  = texture(textures[0], vTexCoord + vec2(0.0, o.y));
  vec4 se = texture(textures[0], vTexCoord + vec2(o.x, o.y));

  vec3 gradY = (sw.rgb + 2.0 * s.rgb + se.rgb) - (nw.rgb + 2.0 * n.rgb + ne.rgb);
  vec3 gradX = (ne.rgb + 2.0 * e.rgb + se.rgb) - (nw.rgb + 2.0 * w.rgb + sw.rgb);
  float edge = dot(gradX, gradX) + dot(gradY, gradY);
#ifdef FX_FIXED_THRESHOLD
  float threshold = 0.64;
#else
  float threshold = fc.g_AntiThreshold.x;
#endif
  outColor = threshold > edge ? c : (nw + n + ne + w + c + e + sw + s + se) * (1.0 / 9.0);
}
