#version 450
#extension GL_GOOGLE_include_directive : require
#include "fx_common.glsl"
// yGamma: raises the colour to a power (the exponent is the constant as given, not its inverse). Alpha is untouched.
layout(set = 0, binding = 1, std140) uniform FragmentConstants { vec4 g_fGamma; } fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 src = texture(textures[0], vTexCoord);
  outColor = vec4(fx_pow(src.rgb, fc.g_fGamma.x), src.a);
}
