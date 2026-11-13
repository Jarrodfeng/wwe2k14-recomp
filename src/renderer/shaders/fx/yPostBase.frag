#version 450
// yPostBase: fills and copies for post-processing targets. Options:
//   FILL         a constant colour (g_f4FillColor), no textures
//   FILL_DEPTH   the constant colour, and the depth taken from the depth texture (slot 0)
//   COPY_DEPTH   colour from slot 1, depth from slot 0 (a copy that also carries the depth over)
#if defined(FILL) || defined(FILL_DEPTH)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 g_f4FillColor;
} fc;
#endif
layout(set = 1, binding = 0) uniform sampler2D textures[16];
#if defined(FILL_DEPTH) || defined(COPY_DEPTH)
layout(location = 0) in vec2 vTexCoord;
#endif
layout(location = 0) out vec4 outColor;

void main() {
#if defined(FILL)
  outColor = fc.g_f4FillColor;
#elif defined(FILL_DEPTH)
  gl_FragDepth = clamp(texture(textures[0], vTexCoord).x, 0.0, 1.0);
  outColor = fc.g_f4FillColor;
#else
  gl_FragDepth = clamp(texture(textures[0], vTexCoord).x, 0.0, 1.0);
  outColor = texture(textures[1], vTexCoord);
#endif
}
