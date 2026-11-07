#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md W2, the wrestlers' materials ours first): the wrestlers' hair material (vertex
// shader 56DD2E94, the yCh_Anisotropic draws; hair / beard by its look, unconfirmed), in matches and in entrances, in
// place of guest pixel shader 58782AADE08EE063, through the shader swap (src\scene_swap.h; SDK
// vulkan/native/shader_swap.h). The draw keeps its pass, textures, samplers and float constants; the colour goes out
// through the draw's own alpha test / exponent bias / gamma step (xenos_ps.glsl, kFlagOwnEpilogue). The alpha it
// writes is the vertex alpha times the hair texture's alpha, so the draw's alpha test is what cuts the strands.
//
// The same family as the body / gear and head shaders (wrestler_body.frag, wrestler_head.frag) and written the same way,
// from the shader's data flow (the game's microcode read locally, checked against the SDK translator's own SPIR-V of it):
// plain multiplies (no Xenos 0 * inf rule, except the few places named Mul0 / PowAbs / InvLength below, where the
// console's answer is kept on purpose), no texture sign switches (every texture is read unsigned, no exponent adjust, no
// fetch-constant LOD bias; the shader's own LOD bias on the colour texture, c201.w, is kept), the cube lookup by direction
// (the guest builds a face coordinate with its `cube` instruction and the translator turns it back into a direction, so
// the direction goes straight in), no rounding nudge on the coordinates, and implicit derivatives.
//
// What differs from the body and head materials: no shadow map, no reflection cube and no paint branch; the surface
// normal comes from one normal map only; and the light is the plain sky / ground ambient plus the ambient cube, times
// the colour. In their place come two strand highlights (the bitangent pushed along the normal by a "shift" texture,
// then a smooth lobe times a power of the sine of the angle to the view, as hair shaders do), each tinted by the hair
// texture's mask, and the same upward-sky rim as the other materials. Names are by how a value is used; what the
// textures hold in the game's art is unconfirmed.
//
// What it computes, in order:
//   1. the textures: the hair map (normal x / y, strand mask, alpha), the colour, the two strand textures (a shift and
//      a glint, both read at the coordinate scaled by c250.w / c251.w) and the arena's lighting map;
//   2. the surface normal: a basis made from the vertex normal and the incoming tangent, offset by the hair map;
//   3. the light: a sky / ground ambient by the up axis plus the ambient cube map along the normal, times the colour;
//   4. a highlight along the half vector, a rim term scaled by the arena's lighting map (as in the head material);
//   5. the two strand highlights, and the upward-sky rim masked by the strand mask;
//   6. the soft add of those, and the fog, with the highlights added back after it.
//
// Float constants (set 1 binding 2): the guest shader's registers in ascending order - 4, 12, 161, 162, 180, 181, 182,
// 187, 189, 190, 195, 197, 201, 204, 205, 218, 249, 250, 251, 253, 254, 255 (22; for the swap registration).
// 249-255 are the compiler's own literal bank (the guest shader loads them itself); what each holds is inferred from how
// it is used and is NOT captured, so they are read from the constants like any other (K_* below say what they appear to be).

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[22]; } fc;
#define C4   fc.c[0]   // highlight colour
#define C12  fc.c[1]   // direction the highlight is measured against
#define C161 fc.c[2]   // x: lighting map power; y: mask mix; z: rim strength
#define C162 fc.c[3]   // z: height-difference gain for the lighting map term
#define C180 fc.c[4]   // up axis
#define C181 fc.c[5]   // ambient colour at the top
#define C182 fc.c[6]   // ambient colour at the bottom
#define C187 fc.c[7]   // ceiling of the ambient cube's colour
#define C189 fc.c[8]   // weight of the sky / ground ambient
#define C190 fc.c[9]   // light colour (times the vertex colour)
#define C195 fc.c[10]  // fog colour
#define C197 fc.c[11]  // xyz: ambient cube colour scale
#define C201 fc.c[12]  // w: LOD bias of the colour texture
#define C204 fc.c[13]  // xyz: rim colour; w: rim power
#define C205 fc.c[14]  // x: highlight gain; y: fresnel power
#define C218 fc.c[15]  // xyz: tangent / bitangent / normal weights of the perturbed normal
#define C249 fc.c[16]  // x: strand 1 shift base; y: strand 2 shift offset; z, w: powers of the two strand highlights
#define C250 fc.c[17]  // xyz: colour of strand highlight 1; w: u scale of the two strand textures
#define C251 fc.c[18]  // xyz: colour of strand highlight 2; w: v scale of the two strand textures
#define C253 fc.c[19]  // x, w: the lobe's smoothing (expected 2 and 3); y: mix weight of the highlight; z: the guest's cube coordinate offset (expected 1.5; not read here)
#define C254 fc.c[20]  // literal bank: x floor / epsilon (expected about 0); y small offset (expected about 0); z expected -1; w: mix weight of the highlight
#define C255 fc.c[21]  // x expected 1; y: offset of the strand shift (expected about -0.5); z expected 0.5; w expected 2

#define K_EPS       C254.x  // floor of the clamps (about 0)
#define K_MINUS_ONE C254.z
#define K_ONE       C255.x
#define K_HALF      C255.z
#define K_TWO       C255.w

// Textures and samplers under the canonical set-3 bindings (1000 + fetch constant, 1300 + for cubes, 1200 + samplers).
layout(set = 3, binding = 1000) uniform texture2DArray t_albedo;    // fetch constant 0: colour
layout(set = 3, binding = 1001) uniform texture2DArray t_hair;      // 1: normal map (x, y), strand mask (z), alpha (w)
layout(set = 3, binding = 1008) uniform texture2DArray t_glint;     // 8: strand texture for the second highlight (x)
layout(set = 3, binding = 1009) uniform texture2DArray t_shift;     // 9: strand shift (x)
layout(set = 3, binding = 1015) uniform texture2DArray t_lightmap;  // 15: the arena's lighting map
layout(set = 3, binding = 1313) uniform textureCube t_ambient;      // 13: ambient (diffuse) cube
layout(set = 3, binding = 1200) uniform sampler s_albedo;
layout(set = 3, binding = 1201) uniform sampler s_hair;
layout(set = 3, binding = 1208) uniform sampler s_glint;
layout(set = 3, binding = 1209) uniform sampler s_shift;
layout(set = 3, binding = 1215) uniform sampler s_lightmap;
layout(set = 3, binding = 1213) uniform sampler s_ambient;

// The guest's interpolators 0, 1, 2, 5, 6 and 7 (3, 4 and 8 are not read).
layout(location = 0) in vec4 in_normal;    // xyz: vertex normal
layout(location = 1) in vec4 in_uv_fog;    // xy: texture coordinate; z: fog amount
layout(location = 2) in vec4 in_view;      // xyz: view vector (eye to surface)
layout(location = 5) in vec4 in_light_pos; // xy: lighting map coordinate; z: height; w: highlight scale
layout(location = 6) in vec4 in_tangent;   // xyz: tangent
layout(location = 7) in vec4 in_vertex;    // xyz: vertex light colour; w: alpha

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

// One strand highlight's lobe for a strand direction and the view direction: a smooth step of (1 + cos), times the
// sine of the angle between them raised to the highlight's power. (Mul0: with odd constants the power can overflow to
// infinity while the smooth step is 0, and the console's answer is 0.)
float StrandLobe(float cos_angle, float power) {
  float t = clamp(cos_angle + K_ONE, 0.0, 1.0);
  float sine = sqrt(abs(K_ONE - cos_angle * cos_angle));
  return Mul0((t * t) * (C253.w - t * C253.x), PowAbs(sine, power));
}

#ifdef LEAN
#define LEAN_POW(x, p) PowAbs(x, p)  // the plain material's own pow (exact: Mul0 kept)
#include "wrestler_lean.glsl"
#endif

void main() {
  vec2 uv = in_uv_fog.xy;
#ifdef LEAN
#endif
  vec3 vertex_normal = in_normal.xyz;
  vec3 view = in_view.xyz;
  vec3 tangent = in_tangent.xyz;
  vec3 bitangent = cross(vertex_normal, tangent);

  // ---- 1. Texture reads ----
  vec4 hair = SAMPLE2D(t_hair, s_hair, uv);                          // x, y: normal map; z: strand mask; w: alpha
  vec3 albedo_tex = SAMPLE2D_BIAS(t_albedo, s_albedo, uv, C201.w).rgb;
  vec2 strand_uv = vec2(C250.w * uv.x, C251.w * uv.y);
  float strand_shift = SAMPLE2D(t_shift, s_shift, strand_uv).x;
  float strand_glint = SAMPLE2D(t_glint, s_glint, strand_uv).x;
#ifndef LEAN
  vec2 lightmap = SAMPLE2D(t_lightmap, s_lightmap, in_light_pos.xy).xy;
#endif
  float strand_mask = hair.z;
  float mask_term = hair.z * K_ONE + C254.y;                         // the mask as the highlight mix reads it

  // ---- 2. The normal ----
  // Offsets from the hair map, unpacked to a signed number (x on the tangent side, y on the bitangent side).
  float u = hair.y * K_TWO + K_MINUS_ONE;
  float v = hair.x * K_TWO + K_MINUS_ONE;
  vec3 normal = PerturbedNormal(tangent, bitangent, vertex_normal, u, v);
  float n_dot_v = dot(normal, view);

  // ---- 3. Light: sky / ground ambient plus the ambient cube, in the vertex's light colour ----
  vec3 ambient_cube = texture(samplerCube(t_ambient, s_ambient), normal).rgb * C197.xyz;
  ambient_cube = min(ambient_cube, C187.xyz);
  float up = dot(-normal, C180.xyz);
  float up_mix = K_HALF * up + K_HALF;                                  // 0 facing up, 1 facing down
  vec3 sky_ground = (C182.xyz - C181.xyz) * up_mix + C181.xyz;
  vec3 light = sky_ground * C189.xyz + ambient_cube;
  vec3 diffuse = (C190.xyz * albedo_tex) * in_vertex.xyz;
  vec3 base = diffuse * light;                                          // the lit hair colour

  // ---- 4. Highlight and rim, as in the head material ----
  // Highlight along the half vector between the normal and the view vector, strongest at grazing angles.
#ifdef LEAN
  vec3 body = base;
  if (C161.z != 0.0) {  // LEAN: rim strength 0 (uniform): body = base; no highlight, no lighting-map fetch
    vec2 lightmap = SAMPLE2D(t_lightmap, s_lightmap, in_light_pos.xy).xy;
    vec3 half_sum = (normal + view) * K_HALF;
    vec3 half_dir = half_sum * InvLength(dot(half_sum, half_sum));
    float spec_dot = max(dot(half_dir, C12.xyz), K_EPS);
    float edge = K_ONE - abs(n_dot_v);                                    // 0 facing the camera, 1 at the silhouette
    float edge_pow = PowAbs(edge, C205.y);
    vec3 spec = (spec_dot * edge_pow) * C4.xyz * C205.x * in_light_pos.w;
  
    // The base plus the highlight, mixed by the two weights, and added back scaled by the rim strength: the rim is the
    // edge term times the arena's lighting map term, lifted by the strand mask.
    vec3 spec_mix = (base + spec) * C253.y - base + (base * (spec + K_ONE)) * C254.w;
    float lightmap_term = max(lightmap.y - in_light_pos.z, K_EPS) * C162.z + lightmap.x;
    float lightmap_pow = PowAbs(lightmap_term, C161.x);
    float mask_mix = K_ONE + C161.y * mask_term;
    float rim_strength = ((edge * lightmap_pow) * mask_mix) * C161.z;
    body = spec_mix * rim_strength + base;
  }
#else
  vec3 half_sum = (normal + view) * K_HALF;
  vec3 half_dir = half_sum * InvLength(dot(half_sum, half_sum));
  float spec_dot = max(dot(half_dir, C12.xyz), K_EPS);
  float edge = K_ONE - abs(n_dot_v);                                    // 0 facing the camera, 1 at the silhouette
  float edge_pow = PowAbs(edge, C205.y);
  vec3 spec = (spec_dot * edge_pow) * C4.xyz * C205.x * in_light_pos.w;

  // The base plus the highlight, mixed by the two weights, and added back scaled by the rim strength: the rim is the
  // edge term times the arena's lighting map term, lifted by the strand mask.
  vec3 spec_mix = (base + spec) * C253.y - base + (base * (spec + K_ONE)) * C254.w;
  float lightmap_term = max(lightmap.y - in_light_pos.z, K_EPS) * C162.z + lightmap.x;
  float lightmap_pow = PowAbs(lightmap_term, C161.x);
  float mask_mix = K_ONE + C161.y * mask_term;
  float rim_strength = ((edge * lightmap_pow) * mask_mix) * C161.z;
  vec3 body = spec_mix * rim_strength + base;
#endif

  // ---- 5. The two strand highlights ----
  // The viewing direction, normalised twice (the guest does, with its small literal offsets in between).
  vec3 view_unit = view * InvLength(dot(view, view)) + vec3(C254.x, C254.y, C254.x);
  vec3 view_dir = view_unit * InvLength(dot(view_unit, view_unit));
  // Each strand direction is the bitangent pushed along the normal by the shift texture (plus its own offset), normalised.
  float shift1 = (C249.x - K_HALF) + strand_shift;
  float shift2 = C249.y + (strand_shift + C255.y);
  vec3 strand1 = bitangent + normal * shift1;
  vec3 strand2 = bitangent + normal * shift2;
  vec3 strand_dir1 = strand1 * InvLength(dot(strand1, strand1) + K_EPS);
  vec3 strand_dir2 = strand2 * InvLength(dot(strand2, strand2));
  float lobe1 = StrandLobe(dot(strand_dir1, view_dir), C249.z);
  float lobe2 = StrandLobe(dot(strand_dir2, view_dir), C249.w);
  vec3 strand_light1 = (C250.xyz * strand_mask) * lobe1;
  vec3 strand_light2 = ((C251.xyz * strand_mask) * strand_glint) * lobe2;

  // The sky seen at a glancing angle in the upward reflection: a rim colour, masked by the strand mask.
  float reflected_y = view.y - normal.y * (n_dot_v + n_dot_v);
  float sky_rim = PowAbs(K_ONE + n_dot_v, C204.w);
  float sky_up = max((-reflected_y) * sky_rim, K_EPS);
  vec3 env = (sky_up * C204.xyz) * strand_mask;

  // ---- 6. Soft add of the highlights and the sky rim, fog (the highlights are added back after it) ----
  vec3 extra = (strand_light1 + strand_light2) + env;
  vec3 soft = body * (K_ONE - min(extra, vec3(K_ONE)));
  vec3 color = (C195.xyz - (extra + soft)) * in_uv_fog.z + soft;
  color = color + strand_light2;
  color = color + strand_light1;
  color = color + env;
  XeOutputColor(vec4(color, in_vertex.w * hair.w));
}
