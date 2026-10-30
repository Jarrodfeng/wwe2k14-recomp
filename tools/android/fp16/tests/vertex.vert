#version 450
// Not a fragment shader: the pass must leave it alone.
layout(location = 0) in vec4 pos;
layout(set = 0, binding = 0) uniform U { mat4 mvp; };
void main() { gl_Position = mvp * pos * 2.0 + vec4(1.0); }
