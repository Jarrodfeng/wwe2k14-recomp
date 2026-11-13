#version 450
// Settings parity S11: the app's overlay (Dear ImGui dialogs): vertex colour times the texture (the font atlas, or plain white).
layout(set = 1, binding = 0) uniform sampler2D overlay_texture;
layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() { out_color = in_color * texture(overlay_texture, in_uv); }
