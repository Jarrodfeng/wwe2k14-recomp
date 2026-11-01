#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the background's bump-mapped lightmapped material (the game's
// yBG_BumpMapLM, guest pixel shader A0A9844B8A45CC6B), through the shader swap (src\scene_swap.h), in order:
//   - the surface normal: the normal map (fetch constant 4, at the colour's coordinate) unpacked as x * c255.x + c254.z,
//     laid on the frame of the third, fourth and fifth interpolators (x on the fourth, z on the third, minus y on the
//     fifth), normalised (the console's reciprocal root of 0 giving 0 kept, InvLength);
//   - the lights: four directional lights, colour c0..c3, direction c8..c11, each term the larger of (minus the
//     normal against its direction) and its floor c74.x..w, summed in the guest's order, times c190, plus c189 times
//     an ambient colour (c181 to c182 by the normal against c180, as x * c254.x + c254.x);
//   - the highlight: the normal against the half vector of the view and the light c8, floored at c254.y, raised to
//     c202.x (PowAbs), times the colour texture's alpha, c201.x, c200 and the first interpolator's z;
//   - the shadow (as arena_bg_shadow_lm.frag, what its terms stand for is unconfirmed): the projected shadow map
//     (fetch constant 12, base level; the fifth interpolator's xy over its w) against a ramp of its z between
//     c168.x and c168.y less c168.z; "height" = c254.w less c169.y times (c254.w less the normal against minus c166,
//     floored at c254.y); term A from the red and green differences with c169.z/w and c170.x..w; term B from the map's
//     blue; shadow m = the larger of A times c168.w and B times c169.x, each times the height;
//   - rgb = (colour texture (fetch constant 0, at the shader's own LOD bias c248.x) times the vertex colour times
//     (c254.w less m)) times the lights, plus the highlight times (c254.w less m), all times the lighting map (fetch
//     constant 13, eighth interpolator's coordinate) scaled by its strength c184.z.
// Alpha = the vertex alpha alone. c254.w is one and c255.x the normal map's scale here (the compiler's literal bank,
// unconfirmed values). The console's 0 x infinity = 0 is kept in the ramp's reciprocal, the projection and the powers.
// Float constants (set 1 binding 2): 0, 1, 2, 3, 8, 9, 10, 11, 74, 166, 168, 169, 170, 180, 181, 182, 184, 189, 190,
// 200, 201, 202, 248, 254, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[25]; } fc;
#define C0   fc.c[0]   // rgb: colour of light 0
#define C1   fc.c[1]   // rgb: colour of light 1
#define C2   fc.c[2]   // rgb: colour of light 2
#define C3   fc.c[3]   // rgb: colour of light 3
#define C8   fc.c[4]   // xyz: direction of light 0 (also the highlight's light)
#define C9   fc.c[5]   // xyz: direction of light 1
#define C10  fc.c[6]   // xyz: direction of light 2
#define C11  fc.c[7]   // xyz: direction of light 3
#define C74  fc.c[8]   // xyzw: floors of lights 0..3
#define C166 fc.c[9]   // xyz: direction of the shadow's height term
#define C168 fc.c[10]  // x, y: the ramp's start and end; z: its offset; w: scale of term A
#define C169 fc.c[11]  // x: scale of term B; y: height term's weight; z: red difference scale; w: its power
#define C170 fc.c[12]  // x: green difference offset; y: its scale; z: its power; w: weight of the green part
#define C180 fc.c[13]  // xyz: the ambient colour's direction
#define C181 fc.c[14]  // rgb: ambient colour at one end
#define C182 fc.c[15]  // rgb: ambient colour at the other end
#define C184 fc.c[16]  // z: strength of the lighting map
#define C189 fc.c[17]  // rgb: weight of the ambient colour
#define C190 fc.c[18]  // rgb: weight of the lights
#define C200 fc.c[19]  // rgb: highlight colour
#define C201 fc.c[20]  // x: highlight strength
#define C202 fc.c[21]  // x: highlight power
#define C248 fc.c[22]  // x: LOD bias of the colour texture
#define C254 fc.c[23]  // x: ambient scale and offset; y: highlight floor; z: normal map offset; w: one
#define C255 fc.c[24]  // x: normal map scale

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0
layout(set = 3, binding = 1004) uniform texture2DArray t_normal;  // 4: the normal map
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;  // 12: the projected shadow map
layout(set = 3, binding = 1013) uniform texture2DArray t_light;   // 13: the lighting map
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1204) uniform sampler s_normal;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1213) uniform sampler s_light;

layout(location = 0) in vec4 in_uv;               // xy: the colour and normal maps' coordinate; z: the highlight's scale
layout(location = 1) in vec4 in_view;             // xyz: towards the camera
layout(location = 2) in vec4 in_frame_n;          // xyz: the frame's third axis (z of the normal map)
layout(location = 3) in vec4 in_frame_t;          // xyz: the frame's first axis (x of the normal map)
layout(location = 4) in vec4 in_frame_b;          // xyz: the frame's second axis (y of the normal map, negated)
layout(location = 5) in vec4 in_shadow;           // xy / w: the shadow map's coordinate; z: the ramp's input
layout(location = 7) in vec4 in_light_uv;         // xy: the lighting map's coordinate
layout(location = 8) centroid in vec4 in_color;   // the vertex colour (centroid sampling, as the guest reads it)

void main() {
  float inv_w = 1.0 / in_shadow.w;
  vec2 shadow_uv = vec2(Mul0(inv_w, in_shadow.x), Mul0(inv_w, in_shadow.y));
  vec3 shadow = textureLod(sampler2DArray(t_shadow, s_shadow), vec3(shadow_uv, 0.0), 0.0).rgb;
  vec3 bump = texture(sampler2DArray(t_normal, s_normal), vec3(in_uv.xy, 0.0)).rgb;
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  vec3 light_map = texture(sampler2DArray(t_light, s_light), vec3(in_light_uv.xy, 0.0)).rgb * C184.z;
  float one = C254.w;

  // The surface normal.
  vec3 unpacked = bump * C255.x + C254.z;
  vec3 raw = unpacked.x * in_frame_t.xyz + (unpacked.z * in_frame_n.xyz - unpacked.y * in_frame_b.xyz);
  vec3 n = raw * InvLength(dot(raw, raw));

  // The highlight.
  vec3 half_sum = in_view.xyz * InvLength(dot(in_view.xyz, in_view.xyz)) - C8.xyz;
  vec3 h = half_sum * InvLength(dot(half_sum, half_sum));
  float highlight = PowAbs(max(dot(n, h), C254.y), C202.x);
  vec3 spec = (C200.xyz * (C201.x * color.a)) * highlight;

  // The shadow.
  float ramp = clamp(Mul0(in_shadow.z - C168.x, 1.0 / (C168.y - C168.x)) - C168.z, 0.0, 1.0);
  float red = clamp((ramp - shadow.x) * C169.z, 0.0, 1.0);
  float green = clamp(((ramp - shadow.y) - C170.x) * C170.y, 0.0, 1.0);
  float x = one - PowAbs(one - red, C169.w);
  float term_a = (x * PowAbs(one - green, C170.z) - x) * C170.w + x;
  float term_b = one - shadow.z;
  float height = one - C169.y * (one - max(dot(n, -C166.xyz), C254.y));
  float shade = max((height * C168.w) * term_a, (height * C169.x) * term_b);
  float lit = one - shade;

  // The lights and the ambient colour.
  float d0 = max(dot(n, -C8.xyz), C74.x);
  float d1 = max(dot(n, -C9.xyz), C74.y);
  float d2 = max(dot(n, -C10.xyz), C74.z);
  float d3 = max(dot(n, -C11.xyz), C74.w);
  vec3 lights = d3 * C3.xyz;
  lights = d2 * C2.xyz + lights;
  lights = d1 * C1.xyz + lights;
  lights = d0 * C0.xyz + lights;
  float ambient_mix = dot(n, C180.xyz) * C254.x + C254.x;
  vec3 ambient = (C182.xyz - C181.xyz) * ambient_mix + C181.xyz;
  vec3 lighting = lights * C190.xyz + C189.xyz * ambient;

  vec3 lit_color = (color.rgb * in_color.rgb) * lit;
  vec3 lit_spec = (spec * in_uv.z) * lit;
  XeOutputColor(vec4(light_map * (lit_color * lighting + lit_spec), in_color.a));
}
