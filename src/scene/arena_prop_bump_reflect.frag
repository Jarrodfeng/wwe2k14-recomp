#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the props' bump-mapped material with an environment reflection
// (the game's yProp_BumpMap, guest pixel shader 741439F55F5B15D4; 6 draws a frame), through the shader swap
// (src\scene_swap.h). The same normal map, four lights and highlight as arena_prop_bump.frag (the highlight's mask is
// the colour texture's alpha), plus a reflection: a cube map read along the reflected view vector, mixed in by a
// Fresnel-style weight and a mask texture. It computes, in order:
//   1. the normal map unpacked (texel * c255.x + c253.z) and weighted onto the vertex normal, the tangent and the
//      (negated) bitangent, then normalised;
//   2. the reflected view vector 2 (N.V) N - V; the guest builds a cube face coordinate from it with its `cube`
//      instruction and the translator turns that back into a direction, so the vector goes straight into the lookup;
//   3. the light: four directional lights c8 / c9 / c10 / c11, each N.(-L) floored at a component of c74 and scaled by
//      colour c0 / c1 / c2 / c3, summed and multiplied by c190, plus a sky / ground term - c181 to c182 by
//      (N.c180 + 1) * c254.x - times c189;
//   4. the highlight: the normal against the half vector of the view and the light c8 (floored at c254.w, raised to
//      c202.x), times the highlight colour c200 times c201.x times the colour texture's alpha, times in0.z;
//   5. the reflection's weight: c208.x times the mask texture, times (1 - max(N.V, c254.w)) to the fourth power scaled
//      by c254.y and offset by c254.z;
//   6. colour = (base colour times vertex colour times light) times (1 - weight) + the cube colour times weight + the
//      highlight; alpha = the vertex alpha.
// The literal bank c253 is read as the guest does (x: zero, so the reflected vector's selects take the view; y: 1; z: the
// map's bias; w: builds the cube coordinate, not read here); what each holds is inferred from use. The console's
// 0 x infinity = 0 is kept where the guest's reciprocal roots and the power meet it (InvLength, PowAbs). A zero view
// vector is not handled like the guest's cube instruction (it picks a fixed face): a view is never zero in a draw.
// Float constants (set 1 binding 2): 0, 1, 2, 3, 8, 9, 10, 11, 74, 180, 181, 182, 189, 190, 200, 201, 202, 208, 248,
// 253, 254, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[22]; } fc;
#define C0   fc.c[0]   // rgb: colour of the light c8
#define C1   fc.c[1]   // rgb: colour of the light c9
#define C2   fc.c[2]   // rgb: colour of the light c10
#define C3   fc.c[3]   // rgb: colour of the light c11
#define C8   fc.c[4]   // xyz: direction of the first light (also the highlight's)
#define C9   fc.c[5]   // xyz: direction of the second light
#define C10  fc.c[6]   // xyz: direction of the third light
#define C11  fc.c[7]   // xyz: direction of the fourth light
#define C74  fc.c[8]   // x, y, z, w: floors of the N.(-L) of lights c8 / c9 / c10 / c11
#define C180 fc.c[9]   // xyz: the sky / ground axis
#define C181 fc.c[10]  // rgb: ground colour
#define C182 fc.c[11]  // rgb: sky colour
#define C189 fc.c[12]  // rgb: strength of the sky / ground term
#define C190 fc.c[13]  // rgb: strength of the four lights
#define C200 fc.c[14]  // rgb: highlight colour
#define C201 fc.c[15]  // x: highlight strength
#define C202 fc.c[16]  // x: highlight power
#define C208 fc.c[17]  // x: strength of the reflection
#define C248 fc.c[18]  // x: LOD bias of the colour texture
#define C253 fc.c[19]  // literal bank: x: 0 (the selects test it), y: 1, z: bias that unpacks the normal map
#define C254 fc.c[20]  // literal bank: x: sky / ground scale, y, z: scale and offset of the Fresnel weight, w: floor of
                       // N.V and of the highlight's dot product
#define C255 fc.c[21]  // literal bank: x: scale that unpacks the normal map

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0 (rgb, and the highlight mask in alpha)
layout(set = 3, binding = 1004) uniform texture2DArray t_normal;  // fetch constant 4
layout(set = 3, binding = 1308) uniform textureCube t_env;        // fetch constant 8: the reflection cube
layout(set = 3, binding = 1009) uniform texture2DArray t_mask;    // fetch constant 9 (red: the reflection's mask)
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1204) uniform sampler s_normal;
layout(set = 3, binding = 1208) uniform sampler s_env;
layout(set = 3, binding = 1209) uniform sampler s_mask;

layout(location = 0) in vec4 in_uv;         // xy: the textures' coordinate; z: scale of the highlight
layout(location = 1) in vec4 in_view;       // xyz: towards the camera
layout(location = 2) in vec4 in_normal;     // xyz: the vertex normal (named by its use in the basis: unconfirmed)
layout(location = 3) in vec4 in_tangent;    // xyz: the tangent (x of the map; unconfirmed)
layout(location = 4) in vec4 in_bitangent;  // xyz: the bitangent (y of the map, negated; unconfirmed)
layout(location = 5) in vec4 in_color;      // the vertex colour and alpha

void main() {
  vec3 map = texture(sampler2DArray(t_normal, s_normal), vec3(in_uv.xy, 0.0)).xyz;
  vec3 u = fma(map, C255.xxx, C253.zzz);
  vec3 bent = fma(vec3(u.x), in_tangent.xyz, fma(vec3(-u.y), in_bitangent.xyz, u.z * in_normal.xyz));
  vec3 n = bent * InvLength(dot(bent, bent));

  vec3 v = in_view.xyz * InvLength(dot(in_view.xyz, in_view.xyz));  // unit vector towards the camera
  float minus_nv = dot(-v.zxy, n.zxy);
  bool zero_x = C253.x == 0.0;
  vec4 select = vec4(zero_x ? -v.z : C253.y, zero_x ? -v.x : C253.y, zero_x ? -v.y : C253.y, C253.y == 0.0 ? -v.y : C253.y);
  float twice = dot(vec4(n.zxy, minus_nv), select);  // -2 N.V with the bank as expected
  vec3 reflected = (-n) * twice - v;
  vec3 env = texture(samplerCube(t_env, s_env), reflected).rgb;

  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  float mask = texture(sampler2DArray(t_mask, s_mask), vec3(in_uv.xy, 0.0)).x;

  vec3 half_sum = v - C8.xyz;
  vec3 h = half_sum * InvLength(dot(half_sum, half_sum));
  float nv = max(dot(n, v), C254.w);
  float spec = PowAbs(max(dot(n, h), C254.w), C202.x);
  float fresnel = C253.y - nv;
  float weight = (C208.x * mask) * fma(fresnel * fresnel * (fresnel * fresnel), C254.y, C254.z);

  vec3 lights = max(dot(n, -C11.xyz), C74.w) * C3.xyz;
  lights = max(dot(n, -C10.xyz), C74.z) * C2.xyz + lights;
  lights = max(dot(n, -C9.xyz), C74.y) * C1.xyz + lights;
  lights = max(dot(n, -C8.xyz), C74.x) * C0.xyz + lights;
  float sky = fma(dot(n, C180.xyz), C254.x, C254.x);
  vec3 ground_sky = (C182.xyz - C181.xyz) * sky + C181.xyz;
  vec3 light = lights * C190.xyz + C189.xyz * ground_sky;

  vec3 highlight = C200.xyz * (C201.x * color.a) * spec * in_uv.z;
  vec3 rgb = (color.rgb * in_color.rgb * light) * (C253.y - weight) + (env * weight + highlight);

  XeOutputColor(vec4(rgb, in_color.a));
}
