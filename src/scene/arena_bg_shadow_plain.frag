#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the background's shadowed material without a highlight (the
// game's yBG_Default, guest pixel shader B63CD5FCA34C147D; 2 draws in the match), through the shader swap
// (src\scene_swap.h). In order: the shadow map (fetch constant 12, an explicit LOD 0 read) is read at the projected
// coordinate (xy over w; the console's 0 x infinity = 0 kept in the division); a distance fade runs from the depth
// (the second interpolator's z) between c168.x and c168.y, minus c168.z, saturated; two shadow strengths come from
// that fade less the map's x and y (the first scaled by c169.z, the second offset by c170.x and scaled by c170.y, both
// saturated); the shadow terms are one minus a power of one minus those strengths (the powers by the guest's exp2 of
// log2, so 0 x infinity = 0 is kept: Mul0), the first blended and weighted by c168.w times one minus the third
// interpolator's z, the second by that one minus times c169.x times one minus the map's z; the larger of the two
// darkens the colour texture (fetch constant 0, at the LOD bias c248.x) times the vertex colour; alpha = the colour's
// times the vertex alpha. What the map's three
// channels hold (shadow kinds, near / far) is unconfirmed. Float constants (set 1 binding 2): 168, 169, 170, 248, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[5]; } fc;
#define C168 fc.c[0]  // x, y: start and end depth of the fade; z: its offset; w: weight of the first shadow term
#define C169 fc.c[1]  // x: weight of the second term's exponent; z: scale of the first strength; w: power of the first
#define C170 fc.c[2]  // x: offset of the second strength; y: its scale; z: power of the second; w: blend of the first
#define C248 fc.c[3]  // x: LOD bias of the colour texture
#define C255 fc.c[4]  // x: 1.0 (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;  // 12: the shadow map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1212) uniform sampler s_shadow;

layout(location = 0) in vec4 in_uv;      // xy: the colour's coordinate
layout(location = 1) in vec4 in_shadow;  // xy, w: the shadow's projected coordinate; z: the depth for the fade
layout(location = 2) in vec4 in_amount;  // z: weights both shadow terms (one minus it)
layout(location = 3) in vec4 in_color;   // the vertex colour

void main() {
  float inv_w = 1.0 / in_shadow.w;
  vec2 shadow_uv = vec2(Mul0(inv_w, in_shadow.x), Mul0(inv_w, in_shadow.y));
  vec3 shadow = textureLod(sampler2DArray(t_shadow, s_shadow), vec3(shadow_uv, 0.0), 0.0).xyz;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  vec3 base = color.rgb * in_color.rgb;

  float fade = clamp(Mul0(in_shadow.z - C168.x, 1.0 / (C168.y - C168.x)) - C168.z, 0.0, 1.0);
  float strength1 = clamp((fade - shadow.x) * C169.z, 0.0, 1.0);
  float strength2 = clamp((fade - shadow.y - C170.x) * C170.y, 0.0, 1.0);

  float weight = C255.x - in_amount.z;
  float power1 = C255.x - exp2(Mul0(log2(abs(C255.x - strength1)), C169.w));
  float power2 = exp2(Mul0(log2(abs(C255.x - strength2)), C170.z));
  float term1 = Mul0(C168.w * weight, Mul0(Mul0(power1, power2) - power1, C170.w) + power1);
  float term2 = Mul0(Mul0(weight, C169.x), C255.x - shadow.z);
  float dark = (term1 >= term2) ? term1 : term2;

  float lit = C255.x - dark;
  vec3 rgb = vec3(Mul0(base.r, lit), Mul0(base.g, lit), Mul0(base.b, lit));
  XeOutputColor(vec4(rgb, color.a * in_color.a));
}
