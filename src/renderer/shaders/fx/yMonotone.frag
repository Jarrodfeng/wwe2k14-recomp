#version 450
// yMonotone: blends the picture towards a single hue. The luma of the pixel is rebuilt into colour with a fixed
// Cb / Cr tint (YCbCr to RGB, BT.601 weights), and the result is mixed over the source by g_fAlpha.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 g_fCb;
  vec4 g_fCr;
  vec4 g_fAlpha;
} fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 src = texture(textures[0], vTexCoord);
#ifdef CLEAN
  // Settings parity S3: FILM GRAIN (30 YEARS) off draws the 30 Years filter's colour wash as the picture itself (binder.h).
  outColor = src;
#else
  float cb = fc.g_fCb.x, cr = fc.g_fCr.x;
  float luma = dot(src.rgb, vec3(0.299, 0.587, 0.114));
  vec3 tinted = vec3(luma + 1.402 * cr,
                     luma - 0.34414 * cb - 0.71414 * cr,
                     luma + 1.772 * cb);
  // Alpha is mixed towards 1 by the same amount.
  outColor = mix(src, vec4(tinted, 1.0), fc.g_fAlpha.x);
#endif
}
