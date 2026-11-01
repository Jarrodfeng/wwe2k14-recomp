#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the wrestlers ours, the last shaders of the frame): the wrestlers' see-through
// material (the yCh_trans effect: parts with a soft alpha, vertex shaders A8E30EED and D345ACFD), the variant that takes
// its reflection from four light lobes instead of a cube map, in matches and in Bryan's entrance, in place of guest pixel
// shader 9864A5F6683152D7 (273 dwords), through the shader swap (src\scene_swap.h; SDK vulkan/native/shader_swap.h). The
// draw keeps its pass, textures, samplers and float constants; the colour goes out through the draw's own alpha test /
// exponent bias / gamma step (xenos_ps.glsl, kFlagOwnEpilogue). The alpha it writes is the normal map's alpha times the
// vertex alpha; the draw's blend state does the see-through.
//
// The same material as wrestler_trans.frag (guest shader CA3E51C5BA993041), written the same way, from the shader's data
// flow (the game's microcode read locally, checked against the SDK translator's own SPIR-V of it): plain multiplies (no
// Xenos 0 * inf rule, except the few places named Mul0 / PowAbs / InvLength below, where the console's answer is kept on
// purpose), no texture sign switches (every texture is read unsigned, with no exponent adjust and no fetch-constant LOD
// bias; the shader's own LOD bias on the colour texture, c201.w, is kept), the ambient cube lookup by direction (the guest
// builds a face coordinate with its `cube` instruction and the translator turns it back into a direction, so the
// direction goes straight in), no rounding nudge on the coordinates, and implicit derivatives.
//
// What differs from wrestler_trans.frag: the reflection. There the reflected view vector reads a reflection cube map
// (fetch constant 11) through a 3x3 matrix; here there is no such texture. The reflected vector is dotted with four
// direction constants (c8, c9, c10, c11), each result is floored and raised to two powers (c142.y and c142.w), and the
// four results of each power are added with the weights in the w of c0 .. c3; the specular mask and the two weights in
// c142.x / c142.z then mix the two sums, exactly where the cube's two channels went. (Four light lobes baked into
// constants is the evident reading; what the game's art calls them is unconfirmed.) The reflection cube's scale (c197.w)
// and the matrix (c52-c54) are not read, and the compiler's literal bank is laid out differently: c254 holds the floor,
// the two highlight weights and the cube coordinate offset; c255 holds the plain numbers and the normal map's scale.
// Everything else is the same.
//
// What it computes, in order:
//   1. the textures: the normal map (offsets x / y, specular mask z, alpha w), the colour (with its LOD bias) and the
//      arena's lighting map;
//   2. the surface normal: a basis made from the vertex normal and the up axis, offset by the normal map;
//   3. the light: a sky / ground ambient by the up axis plus the ambient cube map along the normal, times the colour;
//   4. a highlight along the half vector, a rim term scaled by the arena's lighting map and lifted by the specular mask;
//   5. the four light lobes along the reflected view vector (two powers, weighted and masked by the specular mask), plus
//      the sky seen at a glancing angle in the upward reflection;
//   6. the sum, a soft add of the reflections, and the fog.
//
// Float constants (set 1 binding 2): the guest shader's registers in ascending order - 0, 1, 2, 3, 4, 8, 9, 10, 11, 12,
// 142, 144, 161, 162, 180, 181, 182, 187, 189, 190, 195, 197, 201, 204, 205, 218, 254, 255 (28; the translation's
// float_constants array is 28 too). 254 and 255 are the compiler's own literal bank (the guest shader loads them
// itself); what each holds is inferred from how it is used and is NOT captured, so they are read from the constants like
// any other (K_* below say what they appear to be).

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[28]; } fc;
#define C0   fc.c[0]   // w: weight of lobe 0
#define C1   fc.c[1]   // w: weight of lobe 1
#define C2   fc.c[2]   // w: weight of lobe 2
#define C3   fc.c[3]   // w: weight of lobe 3
#define C4   fc.c[4]   // highlight colour
#define C8   fc.c[5]   // xyz: direction of lobe 0
#define C9   fc.c[6]   // xyz: direction of lobe 1
#define C10  fc.c[7]   // xyz: direction of lobe 2
#define C11  fc.c[8]   // xyz: direction of lobe 3
#define C12  fc.c[9]   // direction the highlight is measured against
#define C142 fc.c[10]  // x, z: weights of the two lobe sums; y, w: their powers
#define C144 fc.c[11]  // colour the reflection amount adds
#define C161 fc.c[12]  // x: lighting map power; y: specular-mask mix; z: rim strength
#define C162 fc.c[13]  // z: height-difference gain for the lighting map term
#define C180 fc.c[14]  // up axis
#define C181 fc.c[15]  // ambient colour at the top
#define C182 fc.c[16]  // ambient colour at the bottom
#define C187 fc.c[17]  // ceiling of the ambient cube's colour
#define C189 fc.c[18]  // weight of the sky / ground ambient
#define C190 fc.c[19]  // light colour (times the vertex colour)
#define C195 fc.c[20]  // fog colour
#define C197 fc.c[21]  // xyz: ambient cube colour scale
#define C201 fc.c[22]  // w: LOD bias of the colour texture
#define C204 fc.c[23]  // xyz: rim colour; w: rim power
#define C205 fc.c[24]  // x: highlight gain; y: fresnel power
#define C218 fc.c[25]  // xyz: tangent / bitangent / normal weights of the perturbed normal
#define C254 fc.c[26]  // literal bank: x, z: mix weights of the highlight; y: small floor / epsilon (expected about 0); w: the guest's cube coordinate offset (expected 1.5; not read here)
#define C255 fc.c[27]  // x expected 1; y: normal map scale; z expected 0.5; w expected -1

#define K_EPS       C254.y  // floor of the clamps (about 0)
#define K_ONE       C255.x
#define K_HALF      C255.z
#define K_MINUS_ONE C255.w

// Textures and samplers under the canonical set-3 bindings (1000 + fetch constant, 1300 + for cubes, 1200 + samplers).
layout(set = 3, binding = 1000) uniform texture2DArray t_albedo;    // fetch constant 0: colour
layout(set = 3, binding = 1001) uniform texture2DArray t_normal;    // 1: normal map (x, y), specular mask (z), alpha (w)
layout(set = 3, binding = 1015) uniform texture2DArray t_lightmap;  // 15: the arena's lighting map
layout(set = 3, binding = 1313) uniform textureCube t_ambient;      // 13: ambient (diffuse) cube
layout(set = 3, binding = 1200) uniform sampler s_albedo;
layout(set = 3, binding = 1201) uniform sampler s_normal;
layout(set = 3, binding = 1215) uniform sampler s_lightmap;
layout(set = 3, binding = 1213) uniform sampler s_ambient;

// The guest's interpolators 0, 1, 2, 5 and 6 (3, 4, 7 and 8 are not read).
layout(location = 0) in vec4 in_normal;    // xyz: vertex normal
layout(location = 1) in vec4 in_uv_fog;    // xy: texture coordinate; z: fog amount
layout(location = 2) in vec4 in_view;      // xyz: view vector (eye to surface)
layout(location = 5) in vec4 in_light_pos; // xy: lighting map coordinate; z: height; w: highlight scale
layout(location = 6) in vec4 in_vertex;    // xyz: vertex light colour; w: alpha

#define SAMPLE2D(tex, smp, uv) texture(sampler2DArray(tex, smp), vec3((uv), 0.0))
#define SAMPLE2D_BIAS(tex, smp, uv, bias) texture(sampler2DArray(tex, smp), vec3((uv), 0.0), (bias))

// The console's multiply: zero times anything (infinity and NaN included) is zero. Plain maths gives 0 * infinity = NaN,
// so the rule is kept at every product here that can meet an infinity: the guest's reciprocal roots and its log2 maths.
float Mul0(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }

// |x| raised to a power, the way the guest writes it: exp2 of (power * log2 |x|). A zero base has log2 = -infinity, and a
// zero power on it must stay 1 (0 * -inf = 0 on the console), not NaN.
float PowAbs(float x, float power) { return exp2(Mul0(power, log2(abs(x)))); }

// 1 / sqrt(|x|), but 0 for a zero input. (The console's multiply gives 0 * infinity = 0, so a zero vector times the
// infinity the guest shader's reciprocal root makes stays 0; with plain multiplies that would be NaN.)
float InvLength(float squared) {
  float a = abs(squared);
  return a > 0.0 ? inversesqrt(a) : 0.0;
}

// The normal built from the tangent frame (tangent, bitangent, vertex normal) with map offsets u (bitangent side)
// and v (tangent side), normalised.
vec3 PerturbedNormal(vec3 tangent, vec3 bitangent, vec3 vertex_normal, float u, float v) {
  vec3 m = tangent * (C218.x * v) + bitangent * (C218.y * u) + vertex_normal * C218.z;
  return m * InvLength(dot(m, m));
}

// One light lobe's strength along the reflected vector at the two powers: the dot product floored at the small floor
// and raised to each power.
vec2 Lobe(vec3 reflected, vec3 direction) {
  float d = max(dot(reflected, direction), K_EPS);
  return vec2(PowAbs(d, C142.y), PowAbs(d, C142.w));
}

void main() {
  vec2 uv = in_uv_fog.xy;
  vec3 vertex_normal = in_normal.xyz;
  vec3 view = in_view.xyz;

  // ---- 1. Texture reads ----
  vec4 normal_tex = SAMPLE2D(t_normal, s_normal, uv);                       // x, y: normal map; z: specular mask; w: alpha
  vec3 albedo_tex = SAMPLE2D_BIAS(t_albedo, s_albedo, uv, C201.w).rgb;
  vec2 lightmap = SAMPLE2D(t_lightmap, s_lightmap, in_light_pos.xy).xy;
  float spec_mask = normal_tex.z;

  // ---- 2. The normal ----
  // A tangent frame from the vertex normal and the up axis (tangent horizontal, bitangent = normal x tangent).
  vec2 n_horizontal = vec2(vertex_normal.z * K_ONE, vertex_normal.x * K_MINUS_ONE);
  float inv_horizontal = InvLength(dot(n_horizontal, n_horizontal) + K_EPS);
  float tangent_x = vertex_normal.z * inv_horizontal;
  float tangent_z = (vertex_normal.x * K_MINUS_ONE) * inv_horizontal;
  vec3 tangent = vec3(tangent_x, 0.0, tangent_z);
  vec3 bitangent = vec3(vertex_normal.y * tangent_z,
                        tangent_x * vertex_normal.z - tangent_z * vertex_normal.x,
                        -(vertex_normal.y * tangent_x));

  // Offsets from the normal map (x on the tangent side, y on the bitangent side).
  float u = normal_tex.y * C255.y + K_ONE;
  float v = normal_tex.x * C255.y + K_ONE;
  vec3 normal = PerturbedNormal(tangent, bitangent, vertex_normal, u, v);
  float n_dot_v = dot(normal, view);
  vec3 reflected = view - normal * (n_dot_v + n_dot_v);

  // ---- 3. Light: sky / ground ambient plus the ambient cube, in the vertex's light colour ----
  vec3 ambient_cube = texture(samplerCube(t_ambient, s_ambient), normal).rgb * C197.xyz;
  ambient_cube = min(ambient_cube, C187.xyz);
  float up = dot(-normal, C180.xyz);
  float up_mix = K_HALF * up + K_HALF;                                      // 0 facing up, 1 facing down
  vec3 sky_ground = (C182.xyz - C181.xyz) * up_mix + C181.xyz;
  vec3 light = sky_ground * C189.xyz + ambient_cube;
  vec3 diffuse = (albedo_tex * C190.xyz) * in_vertex.xyz;
  vec3 base = diffuse * light;                                              // the lit colour

  // ---- 4. Highlight and rim ----
  // Highlight along the half vector between the normal and the view vector, strongest at grazing angles.
  vec3 half_sum = (normal + view) * K_HALF;
  vec3 half_dir = half_sum * InvLength(dot(half_sum, half_sum));
  float spec_dot = max(dot(half_dir, C12.xyz), K_EPS);
  float edge = K_ONE - abs(n_dot_v);                                         // 0 facing the camera, 1 at the silhouette
  float edge_pow = PowAbs(edge, C205.y);
  vec3 spec = (spec_dot * edge_pow) * C4.xyz * C205.x * in_light_pos.w;

  // The base plus the highlight, mixed by the two weights, and added back scaled by the rim strength: the rim is the
  // edge term times the arena's lighting map term, lifted by the specular mask.
  vec3 spec_mix = (base + spec) * C254.z - base + (base * (spec + K_ONE)) * C254.x;
  float lightmap_term = max(lightmap.y - in_light_pos.z, K_EPS) * C162.z + lightmap.x;
  float lightmap_pow = PowAbs(lightmap_term, C161.x);
  float spec_mask_mix = (spec_mask * K_ONE + K_MINUS_ONE) * C161.y + K_ONE;
  float rim_strength = ((edge * lightmap_pow) * spec_mask_mix) * C161.z;
  vec3 body = spec_mix * rim_strength + base;

  // ---- 5. Reflections ----
  // The four light lobes along the reflected view vector: each weighted by the w of its constant, added up per power,
  // then the two sums weighted by the specular mask and c142.x / c142.z.
  vec2 lobe0 = Lobe(reflected, C8.xyz);
  vec2 lobe1 = Lobe(reflected, C9.xyz);
  vec2 lobe2 = Lobe(reflected, C10.xyz);
  vec2 lobe3 = Lobe(reflected, C11.xyz);
  float sum_a = ((lobe3.x * C3.w + lobe2.x * C2.w) + lobe1.x * C1.w) + lobe0.x * C0.w;
  float sum_b = ((lobe3.y * C3.w + lobe2.y * C2.w) + lobe1.y * C1.w) + lobe0.y * C0.w;
  float reflection = (spec_mask * C142.x) * sum_a + (spec_mask * C142.z) * sum_b + K_EPS;

  // The sky seen at a glancing angle in the upward reflection: a rim colour, masked by the specular mask.
  float sky_rim = PowAbs(n_dot_v + K_ONE, C204.w);
  float sky_up = max((-reflected.y) * sky_rim, K_EPS);
  vec3 env = reflection * C144.xyz + (sky_up * C204.xyz) * spec_mask;

  // ---- 6. Sum, soft add of the reflections, fog ----
  vec3 color = body * (K_ONE - min(env, vec3(K_ONE))) + env;
  color = (C195.xyz - color) * in_uv_fog.z + color;
  XeOutputColor(vec4(color, normal_tex.w * in_vertex.w));
}
