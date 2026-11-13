#version 450
// Flat screen rectangles, pixel stage. See docs\renderer\effects\yRect.md.
// Options: FX_GRADATION (a radial colour ramp from the 2D coordinate, times the colour); otherwise the colour passes straight through.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#ifdef FX_GRADATION
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 GradationPointNum;
  vec4 GradationPointColors[12];
  vec4 GradationPointOffsets[3];    // 12 distances packed four to a vector
} fc;
#endif

layout(location = 0) in vec4 inFirst;
#ifdef FX_GRADATION
layout(location = 1) in vec4 inColor;
#endif
layout(location = 0) out vec4 outColor;

#ifdef FX_GRADATION
float pointOffset(int i) {
  vec4 group = fc.GradationPointOffsets[i >> 2];
  int lane = i & 3;
  return lane == 0 ? group.x : (lane == 1 ? group.y : (lane == 2 ? group.z : group.w));
}

// The ramp colour at distance t from the centre: the first point whose offset lies beyond t closes the segment;
// before the first point the first colour holds, past the last point the last colour holds.
vec4 rampAt(int count, float t) {
  int hit = -1;
  for (int i = 0; i < count; ++i) {
    if (pointOffset(i) > t) { hit = i; break; }
  }
  if (hit >= 1) {
    float lo = pointOffset(hit - 1);
    float along = (t - lo) / (pointOffset(hit) - lo);
    return mix(fc.GradationPointColors[hit - 1], fc.GradationPointColors[hit], along);
  }
  return hit == 0 ? fc.GradationPointColors[0] : fc.GradationPointColors[count - 1];
}
#endif

void main() {
#ifdef FX_GRADATION
  int count = int(fc.GradationPointNum.x);
  // With no ramp points the colour is used as it is.
  outColor = count == 0 ? inColor : rampAt(count, 2.0 * length(inFirst.xy - 0.5)) * inColor;
#else
  outColor = inFirst;
#endif
}
