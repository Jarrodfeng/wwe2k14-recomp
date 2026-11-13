#version 450
// Sweat / liquid accumulation map, vertex stage. See docs\renderer\effects\yLiquidMap.md.
// Draws a patch of the body (positions interpolated between two corners of the target by the first attribute) into a liquid map.
// Options: FX_LOCAL_AREA (also gives the patch's coordinates inside LocalArea), FX_PLAIN (a plain full-map quad: position and uv).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

#ifdef FX_PLAIN
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ScreenScale;      // xy scale, zw offset into the map target
} vc;
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outUv;

void main() {
  gl_Position = vec4(inPosition * vc.ScreenScale.xy + vc.ScreenScale.zw, 0.0, 1.0);
  outUv = vec4(inUv, 0.0, 0.0);
}
#else
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 ScreenScale;
  vec4 WorldParam;       // xy one corner of the patch, zw the other
  vec4 ScreenSize;      // xy: size of the map in texels
#ifdef FX_LOCAL_AREA
  vec4 LocalArea;       // xy lower corner, zw upper corner
#endif
} vc;
layout(location = 0) in vec2 inPatch;     // where inside the patch this vertex is, 0..1
layout(location = 1) in vec4 inFade;      // zw: strength inputs for the pixel stage (the game reads this attribute's third and fourth components)
layout(location = 0) out vec4 outUv;
layout(location = 1) out vec4 outParams;

void main() {
  vec2 position = mix(vc.WorldParam.xy, vc.WorldParam.zw, inPatch);
  gl_Position = vec4(position * vc.ScreenScale.xy + vc.ScreenScale.zw, 0.0, 1.0);
  // The second pair of outputs is the position moved by half a texel, to read the previous map.
  vec2 halfTexel = 0.5 / vc.ScreenSize.xy;
#ifdef FX_LOCAL_AREA
  vec2 area = vc.LocalArea.zw - vc.LocalArea.xy;
  vec2 inverseArea = vec2(area.x != 0.0 ? 1.0 / area.x : 0.0, area.y != 0.0 ? 1.0 / area.y : 0.0);   // an empty area gives the game's zero
  outUv = vec4((position - vc.LocalArea.xy) * inverseArea, position + halfTexel);
#else
  outUv = vec4(0.0, 0.0, position + halfTexel);
#endif
  float cube = abs(inPatch.y * inPatch.y * inPatch.y);
  outParams = vec4(cube, 1.0 - fx_pow(1.0 - inFade.w, 100.0), inFade.z, inFade.w);
}
#endif
