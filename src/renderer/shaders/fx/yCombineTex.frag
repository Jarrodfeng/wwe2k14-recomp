#version 450
// yCombineTex: layers four textures over the frame and writes the depth. Slots: 0 frame, 1 depth, 2 additive layer,
// 3 subtractive layer, 4 "normal" layer (blended over the frame by its own alpha).
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 frame = texture(textures[0], vTexCoord);
  vec4 normalLayer = texture(textures[4], vTexCoord);
  vec4 addLayer = clamp(texture(textures[2], vTexCoord), 0.0, 1.0);
  vec4 subLayer = clamp(texture(textures[3], vTexCoord), 0.0, 1.0);
  gl_FragDepth = clamp(texture(textures[1], vTexCoord).x, 0.0, 1.0);
  vec3 base = mix(frame.rgb, normalLayer.rgb, normalLayer.a);
  // Alpha is the frame's alpha plus the additive layer's, minus the subtractive layer's.
  outColor = clamp(vec4(base + addLayer.rgb, frame.a + addLayer.a) - subLayer, 0.0, 1.0);
}
