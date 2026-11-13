#version 450
// Debug lines and polygons, vertex stage. See docs\renderer\effects\yDebugPrimitive.md.
// Options: FX_TEXCOORD (passes a texture coordinate), FX_SHADED (grey shade from the fetched direction, no colour constant); otherwise the Color constant is passed.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 mWVP[4];
#ifndef FX_SHADED
#ifndef FX_TEXCOORD
  vec4 Color;
#endif
#endif
} vc;

layout(location = 0) in vec4 inPosition;
#ifdef FX_SHADED
layout(location = 1) in vec3 inDirection;
#endif
#ifdef FX_TEXCOORD
layout(location = 2) in vec2 inTexCoord;
#endif

layout(location = 0) out vec4 outColor;

void main() {
  gl_Position = vec4(dot(vc.mWVP[0], inPosition), dot(vc.mWVP[1], inPosition), dot(vc.mWVP[2], inPosition), dot(vc.mWVP[3], inPosition));
#ifdef FX_SHADED
  // A fixed light direction (-0.5, 0.5, -0.5 over the fetched xyz) with a floor of 0.2 of ambient.
  float shade = max(dot(inDirection, vec3(-0.5, 0.5, -0.5)), 0.0) + 0.2;
  outColor = vec4(shade, shade, shade, 1.0);
#elif defined(FX_TEXCOORD)
  outColor = vec4(inTexCoord, 0.0, 0.0);
#else
  outColor = vc.Color;
#endif
}
