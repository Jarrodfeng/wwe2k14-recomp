#version 450
// Full-screen quad of the final image passes, vertex stage. See docs\renderer\effects\yGlareEtc.md.
// Options: FX_CORNER_UV (also passes the texture coordinate moved by half a texel of the 1280x720 source, for the corner taps of the edge smoothing).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 0) out vec4 vTexCoord;   // xy the coordinate, zw the shifted one (or zero)

void main() {
  gl_Position = vec4(inPosition, 1.0);
#ifdef FX_CORNER_UV
  vTexCoord = vec4(inTexCoord, inTexCoord - vec2(0.5 / 1280.0, 0.5 / 720.0));
#else
  vTexCoord = vec4(inTexCoord, 0.0, 0.0);
#endif
}
