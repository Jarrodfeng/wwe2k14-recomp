#version 450
// Planar shadow of a character, pixel stage. See docs\renderer\effects\yPlaneShadowMap.md.
// Options: FX_READBACK (reads the shadow map at the projected position, otherwise writes the darkness from the vertex stage).
layout(location = 0) in vec4 inShadow;
layout(location = 0) out vec4 outColor;

#ifdef FX_READBACK
layout(set = 1, binding = 0) uniform sampler2D textures[16];
#endif

void main() {
#ifdef FX_READBACK
  // Only the alpha carries the shadow strength; the colour is black.
  float depth = texture(textures[12], inShadow.xy / inShadow.w).x;
  outColor = vec4(0.0, 0.0, 0.0, clamp(depth, 0.0, 1.0));
#else
  outColor = inShadow;
#endif
}
