#version 450
// Flat screen rectangles (fades, boxes, colour bars), vertex stage. See docs\renderer\effects\yRect.md.
// Options: FX_VERTEX_COLOR (colour from the vertex, scaled by the Color constant), FX_TEXCOORD (passes a 2D coordinate for a gradation; colour moves to the next output).
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 PosScale;
  vec4 Color;
  vec4 fUseProjMatrix;
  vec4 matProj[4];
} vc;

layout(location = 0) in vec3 inPosition;
#ifdef FX_VERTEX_COLOR
layout(location = 1) in vec4 inColor;
#endif
#ifdef FX_TEXCOORD
layout(location = 2) in vec2 inTexCoord;
#endif

layout(location = 0) out vec4 outFirst;
#ifdef FX_TEXCOORD
layout(location = 1) out vec4 outColor;
#endif

void main() {
  if (vc.fUseProjMatrix.x == 0.0) {
    // Pixel-style units: origin shifted half a unit, scaled to clip space, y flipped.
    vec2 scaled = (inPosition.xy - 0.5) * vc.PosScale.xy;
    gl_Position = vec4(scaled.x - 1.0, 1.0 - scaled.y, inPosition.z, 1.0);
  } else {
    vec4 p = vec4(inPosition, 1.0);
    gl_Position = vec4(dot(vc.matProj[0], p), dot(vc.matProj[1], p), dot(vc.matProj[2], p), dot(vc.matProj[3], p));
  }
#ifdef FX_TEXCOORD
  outFirst = vec4(inTexCoord, 0.0, 0.0);
  outColor = vc.Color;
#elif defined(FX_VERTEX_COLOR)
  outFirst = inColor * vc.Color;
#else
  outFirst = vc.Color;
#endif
}
