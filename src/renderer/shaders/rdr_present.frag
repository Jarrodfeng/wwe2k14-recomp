#version 450
// The display curve is a 256-entry table the game sets, applied per channel by value (bilinear between entries is not wanted: texelFetch).
layout(set = 1, binding = 0) uniform sampler2D source_image;   // texture slot 0: every slot is its own binding
layout(set = 1, binding = 1) uniform sampler2D display_curve;  // slot 1
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec3 color = texture(source_image, in_uv).rgb;
  ivec3 index = ivec3(clamp(color, 0.0, 1.0) * 255.0 + 0.5);
  out_color = vec4(texelFetch(display_curve, ivec2(index.r, 0), 0).r,
                   texelFetch(display_curve, ivec2(index.g, 0), 0).g,
                   texelFetch(display_curve, ivec2(index.b, 0), 0).b,
                   1.0);
}
