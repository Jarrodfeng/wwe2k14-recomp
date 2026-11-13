#version 450
// Radial blur, vertex stage. See docs\renderer\effects\yRadialBlur.md.
// A full-screen quad: position and texture coordinate pass straight through.
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
  gl_Position = inPosition;
  vTexCoord = inTexCoord;
}
