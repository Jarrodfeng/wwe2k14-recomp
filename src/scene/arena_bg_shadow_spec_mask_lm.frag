#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the background's lightmapped material with both a masked highlight
// and a projected shadow (the game's yBG_DefaultLM, guest pixel shader 9B46FDF960C4CC1F; 24 draws of the entrance stage;
// the third yBG_DefaultLM variant with a highlight: arena_bg_spec_mask_lm.frag's highlight and arena_bg_shadow_lm.frag's
// shadow in one shader, their maths unchanged), through the shader swap (src\scene_swap.h), in order:
//   - the shadow m as in arena_bg_shadow_lm.frag: the map (fetch constant 12, base level) read at the fourth
//     interpolator's xy divided by its w (the console's 0 x infinity = 0 kept, Mul0), a ramp of its z between c168.x
//     and c168.y less c168.z compared with the map's red and green (terms A and B, c169 / c170; unconfirmed what they
//     stand for), the larger of A times c168.w and B times c169.x, each also times (c255.x minus the fifth
//     interpolator's z);
//   - the colour texture (fetch constant 0, at the shader's own LOD bias c248.x) times the vertex colour;
//   - the highlight colour c200 scaled by c201.x, the colour's alpha and the first interpolator's z, times the
//     highlight: the normal against the half vector of the view and the light c8, floored at c255.y and raised to
//     c202.x (the console's 0 x infinity = 0 is kept in the power, PowAbs);
//   - (colour + highlight) times (c255.x minus m), times the lighting map (fetch constant 13, sixth interpolator's
//     coordinate) scaled by its strength c184.z. Alpha = the vertex alpha alone.
// Float constants (set 1 binding 2): 8, 168, 169, 170, 184, 200, 201, 202, 248, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[10]; } fc;
#define C8   fc.c[0]  // xyz: the light's direction
#define C168 fc.c[1]  // x, y: the ramp's start and end; z: its offset; w: scale of term A
#define C169 fc.c[2]  // x: scale of term B; z: red difference scale; w: its power
#define C170 fc.c[3]  // x: green difference offset; y: its scale; z: its power; w: weight of the green part
#define C184 fc.c[4]  // z: strength of the lighting map
#define C200 fc.c[5]  // xyz: highlight colour
#define C201 fc.c[6]  // x: highlight strength
#define C202 fc.c[7]  // x: highlight power
#define C248 fc.c[8]  // x: LOD bias of the colour texture
#define C255 fc.c[9]  // x: one; y: floor of the highlight's dot product (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;  // 12: the projected shadow map
layout(set = 3, binding = 1013) uniform texture2DArray t_light;   // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_uv;                // xy: the colour's coordinate; z: the highlight's scale
layout(location = 1) in vec4 in_normal;            // xyz: the surface normal
layout(location = 2) in vec4 in_view;              // xyz: towards the camera
layout(location = 3) in vec4 in_shadow;            // xy / w: the shadow map's coordinate; z: the ramp's input
layout(location = 4) in vec4 in_height;            // z: scales both shadow terms (unconfirmed what it is)
layout(location = 5) in vec4 in_light_uv;          // xy: the lighting map's coordinate
layout(location = 6) centroid in vec4 in_color;    // the vertex colour (centroid sampling, as the guest reads it)

// A vector's squared length summed in the guest's order (z squared first, then x, then y, each step a multiply-add).
float Length2(vec3 v) { return fma(v.y, v.y, fma(v.x, v.x, v.z * v.z)); }

// The highlight as the guest writes it (arena_ps.glsl's ArenaHighlight with the guest's own summing order, which
// matters where the dot product is near the floor): the view and the half vector normalised with the 0 x infinity = 0
// kept (InvLength), the dot product summed z, x, y with separate adds.
float Highlight(vec3 normal, vec3 view, vec3 light_dir, float floor_value, float power) {
  precise vec3 view_unit = view * InvLength(Length2(view));
  precise vec3 half_sum = view_unit - light_dir;
  vec3 h = half_sum * InvLength(Length2(half_sum));
  vec3 n = normal * InvLength(Length2(normal));
  precise float dz = n.z * h.z;
  precise float dx = n.x * h.x;
  precise float dy = n.y * h.y;
  precise float d = (dz + dx) + dy;
  return PowAbs(max(d, floor_value), power);
}

void main() {
  float inv_w = 1.0 / in_shadow.w;
  vec2 shadow_uv = vec2(Mul0(inv_w, in_shadow.x), Mul0(inv_w, in_shadow.y));
  vec3 shadow = textureLod(sampler2DArray(t_shadow, s_shadow), vec3(shadow_uv, 0.0), 0.0).rgb;
  vec3 light = texture(sampler2DArray(t_light, s_light), vec3(in_light_uv.xy, 0.0)).rgb * C184.z;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  float one = C255.x;
  vec3 base = color.rgb * in_color.rgb;

  float highlight = Highlight(in_normal.xyz, in_view.xyz, C8.xyz, C255.y, C202.x);
#ifdef MEDIUMP
  mediump float mS = color.a * C201.x;
  mediump vec3 mC200 = C200.xyz;
  mediump float mHl = highlight;
  mediump float mHs = in_uv.z;
  mediump vec3 spec = ((mS * mC200) * mHl) * mHs;
#else
  vec3 spec = (((color.a * C201.x) * C200.xyz) * highlight) * in_uv.z;
#endif

  float ramp = clamp(Mul0(in_shadow.z - C168.x, 1.0 / (C168.y - C168.x)) - C168.z, 0.0, 1.0);
  float red = clamp((ramp - shadow.x) * C169.z, 0.0, 1.0);
  float green = clamp(((ramp - shadow.y) - C170.x) * C170.y, 0.0, 1.0);
  float x = one - PowAbs(one - red, C169.w);
  float term_a = (x * PowAbs(one - green, C170.z) - x) * C170.w + x;
  float term_b = one - shadow.z;
  float height = one - in_height.z;
  float shade = max((C168.w * height) * term_a, (height * C169.x) * term_b);

  float lit = one - shade;
#ifdef MEDIUMP
  mediump vec3 mBase = base, mLight = light;
  mediump float mLit = lit;
  mediump vec3 rgb = mLight * (mBase * mLit + spec * mLit);
#else
  vec3 rgb = light * (base * lit + spec * lit);
#endif
  XeOutputColor(vec4(rgb, in_color.a));
}
