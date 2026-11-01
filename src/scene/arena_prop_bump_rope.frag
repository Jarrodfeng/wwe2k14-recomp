#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the props' bump-mapped material with the highlight mask in the
// colour texture's alpha and no alpha of its own (the game's yProp_BumpMap, guest pixel shader AFB4E9E073971D7A; the
// near ring ropes, 24 draws a frame), through the shader swap (src\scene_swap.h). The same maths as arena_prop_bump.frag
// except for three things: no separate mask texture (the highlight mask is the colour texture's alpha), the output alpha
// is the vertex alpha alone, and the highlight colour is c200 times (c201.x times the mask). The sister shader
// C61723668154F870 (arena_prop_bump_rope2.frag) takes the mask from the normal map's alpha instead.
// What it computes, in order:
//   1. the colour texture at the shader's own LOD bias (c248.x) and the normal map;
//   2. the surface normal: the normal map unpacked (texel * c255.w + c255.z) and weighted onto the vertex normal, the
//      tangent and the (negated) bitangent, then normalised;
//   3. the light: four directional lights c8 / c9 / c10 / c11, each N.(-L) floored at a component of c74 and scaled by
//      colour c0 / c1 / c2 / c3, summed and multiplied by c190, plus a sky / ground term - c181 to c182 by N against c180
//      (halved: (N.c180 + 1) * c255.x) - times c189;
//   4. the highlight: the normal against the half vector of the view and the light c8, floored at c255.y and raised to
//      c202.x, times the highlight colour c200 times c201.x times the mask, times the interpolated scale in0.z;
//   5. colour = the base colour times the vertex colour times the light, plus the highlight; alpha = the vertex alpha.
// The console's 0 x infinity = 0 is kept where the guest's reciprocal roots and the power meet it (InvLength, PowAbs).
// Float constants (set 1 binding 2): 0, 1, 2, 3, 8, 9, 10, 11, 74, 180, 181, 182, 189, 190, 200, 201, 202, 248, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[19]; } fc;
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
#define C248 fc.c[17]  // x: LOD bias of the colour texture
#define C255 fc.c[18]  // the compiler's literal bank: x: sky / ground scale, y: floor of the highlight's dot product,
                       // z, w: bias and scale that unpack the normal map

layout(set = 3, binding = 1000) uniform texture2DArray t_color;   // fetch constant 0 (rgb, and the mask in alpha)
layout(set = 3, binding = 1004) uniform texture2DArray t_normal;  // fetch constant 4
layout(set = 3, binding = 1200) uniform sampler s_color;
layout(set = 3, binding = 1204) uniform sampler s_normal;

layout(location = 0) in vec4 in_uv;         // xy: the textures' coordinate; z: scale of the highlight
layout(location = 1) in vec4 in_view;       // xyz: towards the camera
layout(location = 2) in vec4 in_normal;     // xyz: the vertex normal (named by its use in the basis: unconfirmed)
layout(location = 3) in vec4 in_tangent;    // xyz: the tangent (x of the map; unconfirmed)
layout(location = 4) in vec4 in_bitangent;  // xyz: the bitangent (y of the map, negated; unconfirmed)
layout(location = 5) in vec4 in_color;      // the vertex colour and alpha

void main() {
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  vec3 map = texture(sampler2DArray(t_normal, s_normal), vec3(in_uv.xy, 0.0)).xyz;
  float mask = color.a;

  vec3 u = map * C255.w + C255.z;
  vec3 bent = fma(vec3(u.x), in_tangent.xyz, fma(vec3(-u.y), in_bitangent.xyz, u.z * in_normal.xyz));
  vec3 n = bent * InvLength(dot(bent, bent));

  vec3 half_sum = in_view.xyz * InvLength(dot(in_view.xyz, in_view.xyz)) - C8.xyz;
  vec3 h = half_sum * InvLength(dot(half_sum, half_sum));
  float spec = PowAbs(max(dot(n, h), C255.y), C202.x);

  vec3 lights = max(dot(n, -C11.xyz), C74.w) * C3.xyz;
  lights = max(dot(n, -C10.xyz), C74.z) * C2.xyz + lights;
  lights = max(dot(n, -C9.xyz), C74.y) * C1.xyz + lights;
  lights = max(dot(n, -C8.xyz), C74.x) * C0.xyz + lights;
  float sky = fma(dot(n, C180.xyz), C255.x, C255.x);
  vec3 ground_sky = (C182.xyz - C181.xyz) * sky + C181.xyz;
  vec3 light = lights * C190.xyz + C189.xyz * ground_sky;

  vec3 highlight = C200.xyz * (C201.x * mask) * spec * in_uv.z;

  XeOutputColor(vec4(color.rgb * in_color.rgb * light + highlight, in_color.a));
}
