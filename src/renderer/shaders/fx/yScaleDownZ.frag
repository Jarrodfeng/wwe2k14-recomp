#version 450
// yScaleDownZ: 4-tap box average of the scene depth into a smaller target's depth.
//   DEPTH_ONLY   only the depth texture (slot 0). The colour output is a leftover: red is 1 where the third tap's depth is exactly 0, the rest 0.
//   FRAME_DEPTH  depth (slot 0) and frame (slot 1): colour is the average of the frame taps (alpha 0).
layout(set = 0, binding = 1, std140) uniform FragmentConstants { vec4 sampleOffsets[4]; } fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  float depthSum = 0.0;
  float thirdTap = 0.0;
  for (int i = 0; i < 4; i++) {
    float d = texture(textures[0], vTexCoord + fc.sampleOffsets[i].xy).x;
    depthSum += d;
    if (i == 2) thirdTap = d;
  }
  gl_FragDepth = clamp(depthSum * 0.25, 0.0, 1.0);
#ifdef FRAME_DEPTH
  vec3 colorSum = vec3(0.0);
  for (int i = 0; i < 4; i++) colorSum += texture(textures[1], vTexCoord + fc.sampleOffsets[i].xy).rgb;
  outColor = vec4(colorSum * 0.25, 0.0);
#else
  outColor = vec4(thirdTap == 0.0 ? 1.0 : 0.0, 0.0, 0.0, 0.0);
#endif
}
