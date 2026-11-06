#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the background's lightmapped material darkened by a projected
// shadow map (the game's yBG_DefaultLM, guest pixel shader ED3793930D3A2C1D; the other yBG_DefaultLM variant besides
// arena_bg_spec_mask_lm.frag; what the ramp and the two shadow terms stand for is unconfirmed), through the shader
// swap (src\scene_swap.h), in order:
//   - the shadow map (fetch constant 12, base level) read at the second interpolator's xy divided by its w (the
//     console's 0 x infinity = 0 kept, Mul0); a ramp of the second interpolator's z between c168.x and c168.y, less
//     c168.z, saturated, is compared with the map's red and green;
//   - term A: the red difference times c169.z, saturated, one minus it raised to c169.w, one minus that (x); then
//     x + (x * (one minus the green term raised to c170.z) - x) * c170.w, where the green term is the green
//     difference less c170.x, times c170.y, saturated;
//   - term B: the map's blue turned round (one minus it);
//   - the shadow m = the larger of A times c168.w and B times c169.x, each also times (c255.x minus the third
//     interpolator's z);
//   - the colour texture (fetch constant 0, at the shader's own LOD bias c248.x) times the vertex colour times
//     (c255.x minus m), times the lighting map (fetch constant 13, fourth interpolator's coordinate) scaled by its
//     strength c184.z.
// Alpha = the colour's times the vertex alpha. The console's 0 x infinity = 0 is kept in the ramp and the powers.
// Float constants (set 1 binding 2): 168, 169, 170, 184, 248, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[6]; } fc;
#define C168 fc.c[0]  // x, y: the ramp's start and end; z: its offset; w: scale of term A
#define C169 fc.c[1]  // x: scale of term B; z: red difference scale; w: its power
#define C170 fc.c[2]  // x: green difference offset; y: its scale; z: its power; w: weight of the green part
#define C184 fc.c[3]  // z: strength of the lighting map
#define C248 fc.c[4]  // x: LOD bias of the colour texture
#define C255 fc.c[5]  // x: one (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;  // 12: the projected shadow map
layout(set = 3, binding = 1013) uniform texture2DArray t_light;   // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_uv;               // xy: the colour's coordinate
layout(location = 1) in vec4 in_shadow;           // xy / w: the shadow map's coordinate; z: the ramp's input
layout(location = 2) in vec4 in_height;           // z: scales both shadow terms (unconfirmed what it is)
layout(location = 3) in vec4 in_light_uv;         // xy: the lighting map's coordinate
layout(location = 4) centroid in vec4 in_color;   // the vertex colour (centroid sampling, as the guest reads it)

void main() {
  float inv_w = 1.0 / in_shadow.w;
  vec2 shadow_uv = vec2(Mul0(inv_w, in_shadow.x), Mul0(inv_w, in_shadow.y));
  vec3 shadow = textureLod(sampler2DArray(t_shadow, s_shadow), vec3(shadow_uv, 0.0), 0.0).rgb;
  vec3 light = texture(sampler2DArray(t_light, s_light), vec3(in_light_uv.xy, 0.0)).rgb * C184.z;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  float one = C255.x;

  float ramp = clamp(Mul0(in_shadow.z - C168.x, 1.0 / (C168.y - C168.x)) - C168.z, 0.0, 1.0);
  float red = clamp((ramp - shadow.x) * C169.z, 0.0, 1.0);
  float green = clamp(((ramp - shadow.y) - C170.x) * C170.y, 0.0, 1.0);
  float x = one - PowAbs(one - red, C169.w);
  float term_a = (x * PowAbs(one - green, C170.z) - x) * C170.w + x;
  float term_b = one - shadow.z;
  float height = one - in_height.z;
  float shade = max((C168.w * height) * term_a, (height * C169.x) * term_b);

#ifdef MEDIUMP
  mediump vec3 mCol = color.rgb, mVc = in_color.rgb, mLight = light;
  mediump float mLit = one - shade;
  mediump vec3 rgb = ((mCol * mVc) * mLit) * mLight;
#else
  vec3 rgb = ((color.rgb * in_color.rgb) * (one - shade)) * light;
#endif
  XeOutputColor(vec4(rgb, color.a * in_color.a));
}
