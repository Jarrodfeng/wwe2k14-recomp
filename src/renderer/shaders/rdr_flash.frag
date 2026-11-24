#version 450
// S110 CUTGREY: the crowd camera flash sprite's pixel stage (docs\research\s110\CUTGREY_BUILD.md). The game's pixel stage is `texture * vertex colour` (yFlashDefault_base_fragment).
// The game's depth rule is a WHOLE-SPRITE test: the vertex stage reads the scene depth ONCE at the sprite's centre (vertex texture slot 17) and zeroes the alpha of every vertex when the scene is
// nearer. Every vertex of a sprite uses the same centre, so the result is the same here: one read at the centre's texel (nearest, the game's fetch is unfiltered), the sprite hidden whole.
// fc.a: x, y = the centre's texture coordinate (clamped to the picture, as the game's clamped fetch), z = the centre's own depth (0..1, the viewer's projection), w = 1 when the test is on.
// Our scene depth is the viewer's own 0..1 forward depth and the centre is projected with the same matrix, so "the scene is nearer" is scene < centre.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 a;
} fc;
layout(set = 1, binding = 0) uniform sampler2D flash_map;     // pixel slot 0: the model's flash01 texture
layout(set = 1, binding = 3) uniform sampler2D scene_depth;   // pixel slot 3: the scene's depth

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 out_colour;

void main() {
  if (fc.a.w > 0.5) {
    const ivec2 size = textureSize(scene_depth, 0);
    const ivec2 texel = clamp(ivec2(fc.a.xy * vec2(size)), ivec2(0), size - ivec2(1));
    if (texelFetch(scene_depth, texel, 0).x < fc.a.z) discard;
  }
  out_colour = texture(flash_map, inUv) * inColor;
}
