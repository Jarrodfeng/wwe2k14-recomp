#version 450
// Settings parity S10b: the present with DLSS. DLSS upscaled the picture as it was before the HUD (slot 0, the window's size); the HUD is
// added back as the difference between the final picture and the pre-HUD one at render size (slots 2 and 3, bilinear): where there is no
// HUD the difference is zero and the pixel is DLSS's. Then the game's display curve, as rdr_present.frag. (The old path's composite the same,
// docs\research\dlss_s24.md "The HUD step".)
layout(set = 1, binding = 0) uniform sampler2D dlss_image;     // slot 0
layout(set = 1, binding = 1) uniform sampler2D display_curve;  // slot 1
layout(set = 1, binding = 2) uniform sampler2D final_image;    // slot 2
layout(set = 1, binding = 3) uniform sampler2D before_hud;     // slot 3
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  vec3 hud = texture(final_image, in_uv).rgb - texture(before_hud, in_uv).rgb;
  vec3 color = texture(dlss_image, in_uv).rgb + hud;
  ivec3 index = ivec3(clamp(color, 0.0, 1.0) * 255.0 + 0.5);
  out_color = vec4(texelFetch(display_curve, ivec2(index.r, 0), 0).r,
                   texelFetch(display_curve, ivec2(index.g, 0), 0).g,
                   texelFetch(display_curve, ivec2(index.b, 0), 0).b,
                   1.0);
}
