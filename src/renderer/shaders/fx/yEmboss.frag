#version 450
// yEmboss: grey relief of a single-channel (depth / mask) texture. Six neighbours; the ones towards the top-left add, those
// towards the bottom-right subtract, and the result is centred on 0.5. The centre pixel is not read.
layout(set = 0, binding = 1, std140) uniform FragmentConstants { vec4 g_f2SampleOffsets; } fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  vec2 o = fc.g_f2SampleOffsets.xy;
  float left      = texture(textures[0], vTexCoord + vec2(-o.x, 0.0)).x;
  float right     = texture(textures[0], vTexCoord + vec2(o.x, 0.0)).x;
  float below     = texture(textures[0], vTexCoord + vec2(0.0, o.y)).x;
  float belowRight = texture(textures[0], vTexCoord + vec2(o.x, o.y)).x;
  float aboveLeft = texture(textures[0], vTexCoord + vec2(-o.x, -o.y)).x;
  float above     = texture(textures[0], vTexCoord + vec2(0.0, -o.y)).x;
  float relief = left + aboveLeft + above - right - below - belowRight;
  outColor = vec4(vec3(0.5 - relief), 1.0);
}
