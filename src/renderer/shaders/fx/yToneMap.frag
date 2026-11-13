#version 450
// yToneMap: measures the average scene brightness (auto exposure) in three steps, each shrinking the previous result.
//   INITIAL    9 taps of the scene colour; each tap's luma is turned into its natural log and the nine logs averaged.
//   ITERATIVE  16 taps of the one-channel result, averaged (the value is copied to red, green and blue).
//   FINAL      16 taps taken at absolute coordinates (the offsets themselves, plus half a texel of the 16x16 target, not uv + offset),
//              averaged and turned back with e^x. The result is the log-average luma of the scene.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef INITIAL
  vec4 g_avSampleOffsets[9];
#else
  vec4 g_avSampleOffsets[16];
#endif
} fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
#if defined(INITIAL)
  float logSum = 0.0;
  for (int i = 0; i < 9; i++) {
    vec3 c = texture(textures[0], vTexCoord + fc.g_avSampleOffsets[i].xy).rgb;
    float luma = dot(c, vec3(0.2125, 0.7154, 0.0721)) + 0.0001;
    logSum += log2(abs(luma));
  }
  outColor = vec4(vec3(logSum * (0.693147 * 0.111111)), 1.0);
#elif defined(ITERATIVE)
  float sum = 0.0;
  for (int i = 0; i < 16; i++) sum += texture(textures[0], vTexCoord + fc.g_avSampleOffsets[i].xy).x;
  outColor = vec4(vec3(sum * 0.0625), 1.0);
#else
  float sum = 0.0;
  for (int i = 0; i < 16; i++) sum += texture(textures[0], fc.g_avSampleOffsets[i].xy + 0.03125).x;
  outColor = vec4(vec3(exp2(sum * 0.0901684)), 1.0);
#endif
}
