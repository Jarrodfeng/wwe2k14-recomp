#version 450
// yComposition: small edit passes used when compositing a texture. One define per variant:
//   RGB     copy the colour, alpha 1
//   ALPHA   the source alpha shown as grey, alpha 1
//   OFFSET  copy, reading from uv + g_Offset
//   SPLIT   false-colour a 4-channel mask (channel R, G, B, A of the source): the first non-zero channel in that order
//           gives its g_Comp* colour scaled by the channel value; all four zero gives red = 1 and the rest 0.
#if defined(OFFSET) || defined(SPLIT)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 g_Offset;
#ifdef SPLIT
  vec4 g_CompR;
  vec4 g_CompG;
  vec4 g_CompB;
  vec4 g_CompA;
#endif
} fc;
#endif
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
#if defined(RGB)
  outColor = vec4(texture(textures[0], vTexCoord).rgb, 1.0);
#elif defined(ALPHA)
  outColor = vec4(vec3(texture(textures[0], vTexCoord).a), 1.0);
#elif defined(OFFSET)
  outColor = texture(textures[0], vTexCoord + fc.g_Offset.xy);
#else
  vec4 m = texture(textures[0], vTexCoord + fc.g_Offset.xy);
  if (m.x != 0.0)      outColor = vec4(m.x * fc.g_CompR.xyz, 1.0);
  else if (m.y != 0.0) outColor = vec4(m.y * fc.g_CompG.xyz, 1.0);
  else if (m.z != 0.0) outColor = vec4(m.z * fc.g_CompB.xyz, 1.0);
  else if (m.w != 0.0) outColor = vec4(m.w * fc.g_CompA.xyz, 1.0);
  else                 outColor = vec4(1.0, 0.0, 0.0, 0.0);
#endif
}
