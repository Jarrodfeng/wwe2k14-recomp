#version 450
// Debug lines and polygons, pixel stage. See docs\renderer\effects\yDebugPrimitive.md.
// Options: FX_TEXTURE (the colour is the texture at the passed coordinate); otherwise the colour passes straight through.
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"

layout(set = 1, binding = 0) uniform sampler2D textures[16];

layout(location = 0) in vec4 inFirst;
layout(location = 0) out vec4 outColor;

void main() {
#ifdef FX_TEXTURE
  outColor = texture(textures[0], inFirst.xy);
#else
  outColor = inFirst;
#endif
}
