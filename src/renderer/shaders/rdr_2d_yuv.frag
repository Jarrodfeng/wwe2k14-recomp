#version 450
// Session 101 lane 23 (our 2D pass): a movie's decoded planes drawn as one quad. Planar video: Y, cR and cB in three single-channel textures
// (BT.601, limited range: Y 16..235, chroma 16..240), an optional alpha plane in the fourth slot (fc.mode.x = 1). Our own shader; the conversion is
// the standard BT.601 one (the game's ySprite yuv shader uses the same constants, docs\renderer\effects\ySprite.md).
// Planes may be padded (the game's are): each plane's uv is the movie's uv scaled onto the valid part of the image (plane[p].xy = valid / size) and kept
// half a texel inside it (plane[p].zw = half a texel), so a linear tap never reads padding.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 mode;      // x: 1 = the alpha plane is real, 0 = alpha 1
  vec4 plane[4];  // Y, Cb, Cr, A (the game's slot order)
} fc;
layout(set = 1, binding = 0) uniform sampler2D plane_y;
layout(set = 1, binding = 1) uniform sampler2D plane_cb;   // S101 lane 131 (review F12): the game's slot order is Y, Cb, Cr, A (docs\renderer\MOVIE_OURS.md); this file had Cr and Cb the other way round
layout(set = 1, binding = 2) uniform sampler2D plane_cr;
layout(set = 1, binding = 3) uniform sampler2D plane_a;
layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

vec2 plane_uv(int p) { return clamp(in_uv * fc.plane[p].xy, fc.plane[p].zw, fc.plane[p].xy - fc.plane[p].zw); }

void main() {
  float y = (texture(plane_y, plane_uv(0)).x - 16.0 / 255.0) * (255.0 / 219.0);
  float u = texture(plane_cb, plane_uv(1)).x - 128.0 / 255.0;  // Cb
  float v = texture(plane_cr, plane_uv(2)).x - 128.0 / 255.0;  // Cr
  u *= 255.0 / 224.0;
  v *= 255.0 / 224.0;
  vec3 rgb = vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);
  float a = fc.mode.x > 0.5 ? texture(plane_a, plane_uv(3)).x : 1.0;
  out_color = vec4(clamp(rgb, 0.0, 1.0), a) * in_color;
}
