#version 450
// Depth pre-pass, pixel stage: no colour of its own. See docs\renderer\effects\yZPass.md.
// Options: FX_DEPTH_OVERRIDE (writes a constant depth for every pixel instead of the rasterised one).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#ifdef FX_DEPTH_OVERRIDE
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Depth;               // x
} fc;
#endif

layout(location = 0) out vec4 outColor;

void main() {
  outColor = vec4(0.0);
#ifdef FX_DEPTH_OVERRIDE
  gl_FragDepth = clamp(fc.Depth.x, 0.0, 1.0);
#endif
}
