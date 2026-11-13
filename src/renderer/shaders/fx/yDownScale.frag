#version 450
// yDownScale: shrinks / copies the scene buffer. Options (one define per variant):
//   BOX16       16-tap average, offsets g_avSampleOffsets[0..15]
//   BOX16_Z     same, and the depth texture (slot 1) is averaged into alpha
//   BOX4        4-tap average
//   BOX4_DOWN   4-tap average, colour reduced by g_fDownColor, alpha untouched
//   DOF         4-tap average that skips taps belonging to a different depth layer
//   COPY        plain copy; COPY_Z also rewrites scene depth from the depth texture
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

#if defined(BOX16) || defined(BOX16_Z) || defined(BOX4) || defined(BOX4_DOWN) || defined(DOF)
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#if defined(BOX16) || defined(BOX16_Z)
  vec4 g_avSampleOffsets[16];
#elif defined(BOX4) || defined(BOX4_DOWN) || defined(DOF)
  vec4 g_avSampleOffsets[4];
#endif
#ifdef BOX4_DOWN
  vec4 g_fDownColor;
#endif
#ifdef DOF
  vec4 g_f4DOFParam2;   // x,y = accepted blur-depth range, w = depth tolerance
#endif
} fc;
#endif

layout(set = 1, binding = 0) uniform sampler2D textures[16];

#if !defined(COPY) && !defined(COPY_Z)
vec2 tapUV(int i) { return vTexCoord + fc.g_avSampleOffsets[i].xy; }
#endif

void main() {
#if defined(BOX16)
  vec4 sum = vec4(0.0);
  for (int i = 0; i < 16; i++) sum += texture(textures[0], tapUV(i));
  outColor = sum * 0.0625;

#elif defined(BOX16_Z)
  vec4 sum = vec4(0.0);
  for (int i = 0; i < 16; i++) {
    vec2 uv = tapUV(i);
    sum += vec4(texture(textures[0], uv).rgb, texture(textures[1], uv).x);
  }
  outColor = sum * 0.0625;

#elif defined(BOX4) || defined(BOX4_DOWN)
  vec4 sum = texture(textures[0], tapUV(3));
  sum = texture(textures[0], tapUV(2)) + sum;
  sum = texture(textures[0], tapUV(1)) + sum;
  sum = texture(textures[0], tapUV(0)) + sum;
#ifdef BOX4_DOWN
  outColor = vec4(sum.rgb * 0.25 - fc.g_fDownColor.x, sum.a * 0.25);
#else
  outColor = sum * 0.25;
#endif

#elif defined(DOF)
  // Tap 0 is always used. Each other tap joins the average only when it is not
  // nearer to the camera than the centre pixel minus the tolerance.
  float centreFar = 1.0 - texture(textures[1], vTexCoord).x;
  float threshold = (1.0 - fc.g_f4DOFParam2.w) - texture(textures[1], vTexCoord).x;
  vec3 colour0 = texture(textures[0], tapUV(0)).rgb;
  vec3 colour1 = texture(textures[0], tapUV(1)).rgb;
  bool use1 = (1.0 - texture(textures[1], tapUV(1)).x) >= threshold;
  bool use2 = (1.0 - texture(textures[1], tapUV(2)).x) >= threshold;
  bool use3 = (1.0 - texture(textures[1], tapUV(3)).x) >= threshold;
  vec3 sum = colour1 + colour0;
  float count = use1 ? 2.0 : 1.0;
  if (!use1) sum = colour0;
  if (use2) { sum = texture(textures[0], tapUV(2)).rgb + sum; count += 1.0; }
  if (use3) { sum = texture(textures[0], tapUV(3)).rgb + sum; count += 1.0; }
  // Alpha flags whether the centre pixel lies inside the focus-blur depth band.
  bool inBand = centreFar >= fc.g_f4DOFParam2.x && fc.g_f4DOFParam2.y >= centreFar;
  outColor = vec4((1.0 / count) * sum, inBand ? 1.0 : 0.0);

#elif defined(COPY) || defined(COPY_Z)
  outColor = texture(textures[0], vTexCoord);
#ifdef COPY_Z
  gl_FragDepth = clamp(texture(textures[1], vTexCoord).x, 0.0, 1.0);
#endif
#endif
}
