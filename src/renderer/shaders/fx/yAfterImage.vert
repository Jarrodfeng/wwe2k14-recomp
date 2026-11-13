#version 450
// yAfterImage: full-screen quad. Position and texture coordinate pass straight through.
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
  gl_Position = inPosition;
  vTexCoord = inTexCoord;
}
