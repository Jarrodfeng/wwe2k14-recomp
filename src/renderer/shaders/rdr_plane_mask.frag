#version 450
// Session 101 lane 159 (docs\renderer\SHADOW_PLANE.md): the plane shadow mask, pixel stage. Our own shader: the darkness the vertex stage computed, written as is.
layout(set = 0, binding = 1, std140) uniform FragmentConstants { vec4 Unused; } fc;   // (the binder always has a block; the mask reads none)
layout(location = 0) in vec4 in_shadow;
layout(location = 0) out vec4 out_color;
void main() { out_color = in_shadow; }
