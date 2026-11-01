#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the background's shadowed material with a highlight (the game's
// yBG_Default, guest pixel shader E98D0C3605046302; 4 draws in the match, the floor among them), through the shader
// swap (src\scene_swap.h): arena_bg_shadow_plain.frag's shadow maths (the shadow map, fetch constant 12, read at the
// projected coordinate at LOD 0; the distance fade of the depth between c168.x and c168.y; the two strengths and
// their powers; the larger of the two shadow terms is "dark") plus the props' highlight (ArenaHighlight: the normal
// against the half vector of the view and the light c8, floored at c255.y and raised to c202.x) in the colour c200
// scaled by the colour texture's alpha times c201.x and by the first interpolator's z. The colour texture (fetch
// constant 0, at the LOD bias c248.x) times the vertex colour and the highlight are both multiplied by one minus
// dark and added: rgb = base x (1 - dark) + highlight x (1 - dark). Alpha = the vertex alpha only. What the map's
// three channels hold (shadow kinds, near / far) is unconfirmed. Float constants (set 1 binding 2): 8, 168, 169, 170,
// 200, 201, 202, 248, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[9]; } fc;
#define C8   fc.c[0]  // xyz: the light's direction
#define C168 fc.c[1]  // x, y: start and end depth of the fade; z: its offset; w: weight of the first shadow term
#define C169 fc.c[2]  // x: weight of the second term's exponent; z: scale of the first strength; w: power of the first
#define C170 fc.c[3]  // x: offset of the second strength; y: its scale; z: power of the second; w: blend of the first
#define C200 fc.c[4]  // xyz: highlight colour
#define C201 fc.c[5]  // x: highlight strength (times the colour texture's alpha)
#define C202 fc.c[6]  // x: highlight power
#define C248 fc.c[7]  // x: LOD bias of the colour texture
#define C255 fc.c[8]  // x: 1.0; y: floor of the highlight's dot product (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;  // 12: the shadow map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1212) uniform sampler s_shadow;

layout(location = 0) in vec4 in_uv;      // xy: the colour's coordinate; z: scale of the highlight
layout(location = 1) in vec4 in_normal;  // xyz: the surface normal
layout(location = 2) in vec4 in_view;    // xyz: towards the camera
layout(location = 3) in vec4 in_shadow;  // xy, w: the shadow's projected coordinate; z: the depth for the fade
layout(location = 4) in vec4 in_amount;  // z: weights both shadow terms (one minus it)
layout(location = 5) in vec4 in_color;   // the vertex colour

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

  float highlight = ArenaHighlight(in_normal.xyz, in_view.xyz, C8.xyz, C255.y, C202.x);
  float tint_scale = color.a * C201.x;
  vec3 tint = vec3(Mul0(tint_scale, C200.x), Mul0(tint_scale, C200.y), Mul0(tint_scale, C200.z));
  vec3 spec = vec3(Mul0(tint.x, highlight), Mul0(tint.y, highlight), Mul0(tint.z, highlight));
  spec = vec3(Mul0(spec.x, in_uv.z), Mul0(spec.y, in_uv.z), Mul0(spec.z, in_uv.z));
  spec = vec3(Mul0(spec.x, lit), Mul0(spec.y, lit), Mul0(spec.z, lit));
  vec3 rgb = vec3(Mul0(base.r, lit), Mul0(base.g, lit), Mul0(base.b, lit)) + spec;
  XeOutputColor(vec4(rgb, in_color.a));
}
