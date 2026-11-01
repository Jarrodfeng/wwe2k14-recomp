#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md W2, the wrestlers' materials ours first): the wrestlers' reflective material
// (effect yCh_Reflect) in matches, in place of guest pixel shader 3C06B3329BE81996, through the shader swap
// (src\scene_swap.h; SDK vulkan/native/shader_swap.h). The draw keeps its pass, textures, samplers and float constants;
// the colour goes out through the draw's own alpha test / exponent bias / gamma step (xenos_ps.glsl, kFlagOwnEpilogue).
//
// The same family as the body / gear material (wrestler_body.frag, guest shader 1B66FF87A6433D53) and written the same
// way, from the shader's data flow (the game's microcode read locally, checked against the SDK translator's own SPIR-V
// of it): plain multiplies (no Xenos 0 * inf rule, except the few places named Mul0 / PowAbs / InvLength below, where
// the console's answer is kept on purpose), no texture sign switches (every texture is read unsigned, with no exponent
// adjust and no fetch-constant LOD bias), cube lookups by direction (the guest builds a face coordinate with its `cube`
// instruction and the translator turns it back into a direction, so the direction goes straight in), no rounding nudge
// on the coordinates, and implicit derivatives.
//
// What is gone compared with the body material: the detail texture, the paint mask (its four taps, its tint, its second
// normal and second reflection) and the vertex's blend between two material sets. What is new: a second cube map read
// along a direction the vertex shader sends (interpolator 8), masked by the y channel of a mask texture and added to the
// reflection term, and a scale (c247.z) on the sky / ground ambient. The vertex colour comes in interpolator 6 here
// (interpolator 8 in the body material). Which parts of a wrestler draw with it is unconfirmed (belts / shiny gear?).
//
// What it computes, in order:
//   1. the colour texture, and the specular mask / first normal map (one texture);
//   2. the surface normal: a basis made from the vertex normal and the up axis, one normal map;
//   3. the shadow: the shadow map compared against the pixel's light-space depth, two filter pairs, faded with N.L;
//   4. the light: a sky / ground ambient by the up axis plus the ambient cube map along the normal, times the shadow;
//   5. a highlight along the half vector, a rim term scaled by the arena's lighting map, a reflection cube map along
//      the reflected view vector, and the second cube map (the sent direction) masked by the mask texture;
//   6. the sum, a soft add of the reflections, and the fog.
//
// Float constants (set 1 binding 2): the guest shader's registers in ascending order - 4, 12, 52, 53, 54, 142, 144, 161,
// 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 204, 205, 218, 247, 254, 255 (27; the translation's
// float_constants array is 27 too). 254 and 255 are the compiler's own literal bank (the guest shader loads them itself);
// what each holds is inferred from how it is used and is NOT captured, so they are read from the constants like any
// other (K_* below say what they appear to be).

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[27]; } fc;
#define C4   fc.c[0]   // specular colour
#define C12  fc.c[1]   // direction the highlight is measured against
#define C52  fc.c[2]   // rows of the matrix that takes the reflected vector into the reflection cube's space
#define C53  fc.c[3]
#define C54  fc.c[4]
#define C142 fc.c[5]   // x, z: the reflection cube's two channel weights
#define C144 fc.c[6]   // colour the reflection amount adds
#define C161 fc.c[7]   // x: lighting map power; y: specular-mask mix; z: rim strength
#define C162 fc.c[8]   // z: height-difference gain for the lighting map term
#define C166 fc.c[9]   // key light direction (for the shadow fade)
#define C168 fc.c[10]  // x, y: shadow depth range; z: depth bias; w: shadow strength
#define C169 fc.c[11]  // x unused; y: N.L fade; z: pair 1 gain; w: pair 1 power
#define C170 fc.c[12]  // x: pair 2 offset; y: pair 2 gain; z: pair 2 power; w: pair 2 mix
#define C180 fc.c[13]  // up axis
#define C181 fc.c[14]  // ambient colour at the top
#define C182 fc.c[15]  // ambient colour at the bottom
#define C187 fc.c[16]  // ceiling of the ambient cube's colour
#define C189 fc.c[17]  // weight of the sky / ground ambient
#define C190 fc.c[18]  // light colour (times the vertex colour)
#define C195 fc.c[19]  // fog colour
#define C197 fc.c[20]  // xyz: ambient cube colour scale; w: reflection cube scale
#define C204 fc.c[21]  // xyz: rim colour; w: rim power
#define C205 fc.c[22]  // x: highlight gain; y: fresnel power
#define C218 fc.c[23]  // xyz: tangent / bitangent / normal weights of the perturbed normal
#define C247 fc.c[24]  // z: scale of the sky / ground ambient's weight (unconfirmed what it is for)
#define C254 fc.c[25]  // literal bank: x expected 0 (floor), y, w the highlight's mix weights, z the normal map's scale
#define C255 fc.c[26]  // literal bank: x expected 1, y 1.5 (cube coordinate offset), z -1, w 0.5

#define K_EPS       C254.x  // floor of the clamps (about 0)
#define K_ONE       C255.x
#define K_MINUS_ONE C255.z
#define K_HALF      C255.w

// Textures and samplers under the canonical set-3 bindings (1000 + fetch constant, 1300 + for cubes, 1200 + samplers).
layout(set = 3, binding = 1000) uniform texture2DArray t_albedo;      // fetch constant 0: colour
layout(set = 3, binding = 1001) uniform texture2DArray t_normal1;     // 1: normal map (x, y) and specular mask (w)
layout(set = 3, binding = 1006) uniform texture2DArray t_env_mask;    // 6: where the second cube map shows (y)
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;      // 12: shadow map (four depths)
layout(set = 3, binding = 1015) uniform texture2DArray t_lightmap;    // 15: the arena's lighting map
layout(set = 3, binding = 1310) uniform textureCube t_env;            // 10: second cube map, read along interpolator 8
layout(set = 3, binding = 1311) uniform textureCube t_reflect;        // 11: reflection cube
layout(set = 3, binding = 1313) uniform textureCube t_ambient;        // 13: ambient (diffuse) cube
layout(set = 3, binding = 1200) uniform sampler s_albedo;
layout(set = 3, binding = 1201) uniform sampler s_normal1;
layout(set = 3, binding = 1206) uniform sampler s_env_mask;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1215) uniform sampler s_lightmap;
layout(set = 3, binding = 1210) uniform sampler s_env;
layout(set = 3, binding = 1211) uniform sampler s_reflect;
layout(set = 3, binding = 1213) uniform sampler s_ambient;

// The guest's interpolators 0-3, 5, 6 and 8 (4 and 7 are not read).
layout(location = 0) in vec4 in_normal;     // xyz: vertex normal
layout(location = 1) in vec4 in_uv_fog;     // xy: texture coordinate; z: fog amount
layout(location = 2) in vec4 in_view;       // xyz: view vector (eye to surface)
layout(location = 3) in vec4 in_shadow_pos; // xy / w: shadow map coordinate; z: light-space depth
layout(location = 5) in vec4 in_light_pos;  // xy: lighting map coordinate; z: height; w: highlight scale
layout(location = 6) in vec4 in_vertex;     // xyz: vertex light colour; w: alpha
layout(location = 8) in vec4 in_env_dir;    // xyz: direction of the second cube map (y is flipped by K_MINUS_ONE)

#define SAMPLE2D(tex, smp, uv) texture(sampler2DArray(tex, smp), vec3((uv), 0.0))

// The console's multiply: zero times anything (infinity and NaN included) is zero. Plain maths gives 0 * infinity = NaN,
// so the rule is kept at the few products here that can meet an infinity: the guest's reciprocals and its log2 maths.
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

void main() {
  vec2 uv = in_uv_fog.xy;
  vec3 vertex_normal = in_normal.xyz;
  vec3 view = in_view.xyz;

  // ---- 1. Texture reads at the model coordinate ----
  vec3 albedo_tex = SAMPLE2D(t_albedo, s_albedo, uv).rgb;
  vec4 normal1 = SAMPLE2D(t_normal1, s_normal1, uv);  // x, y: normal map; w: specular mask
  float env_mask = SAMPLE2D(t_env_mask, s_env_mask, uv).y;
  vec2 lightmap = SAMPLE2D(t_lightmap, s_lightmap, in_light_pos.xy).xy;

  // ---- 2. The normal ----
  // A tangent frame from the vertex normal and the up axis (tangent horizontal, bitangent = normal x tangent).
  vec2 n_horizontal = vec2(vertex_normal.z, vertex_normal.x * K_MINUS_ONE);
  float inv_horizontal = InvLength(dot(n_horizontal, n_horizontal));
  float tangent_x = vertex_normal.z * inv_horizontal;
  float tangent_z = (vertex_normal.x * K_MINUS_ONE) * inv_horizontal;
  vec3 tangent = vec3(tangent_x, 0.0, tangent_z);
  vec3 bitangent = vec3(vertex_normal.y * tangent_z,
                        tangent_x * vertex_normal.z - tangent_z * vertex_normal.x,
                        -(vertex_normal.y * tangent_x));
  float base_u = normal1.y * C254.z + K_ONE;
  float base_v = normal1.x * C254.z + K_ONE;
  vec3 normal = PerturbedNormal(tangent, bitangent, vertex_normal, base_u, base_v);

  // ---- Reflection vector ----
  float n_dot_v = dot(normal, view);
  vec3 reflected = view - normal * (n_dot_v + n_dot_v);

  // ---- 3. Shadow ----
  float n_dot_l = max(dot(normal, C166.xyz), K_EPS);
  float shadow_scale = 1.0 / in_shadow_pos.w;
  vec2 shadow_uv = vec2(Mul0(in_shadow_pos.x, shadow_scale), Mul0(in_shadow_pos.y, shadow_scale));
  vec4 occluder = textureLod(sampler2DArray(t_shadow, s_shadow), vec3(shadow_uv, 0.0), 0.0);
  float receiver = clamp(Mul0(in_shadow_pos.z - C168.x, 1.0 / (C168.y - C168.x)) - C168.z, 0.0, 1.0);
  vec4 depth_diff = vec4(receiver) - occluder;
  // Pair 1 (x, z channels): saturated difference, flipped, raised to a power, flipped back.
  float sat_x = clamp(depth_diff.x * C169.z, 0.0, 1.0);
  float sat_z = clamp(depth_diff.z * C169.z, 0.0, 1.0);
  float shade_x = K_ONE - PowAbs(K_ONE - sat_x, C169.w);
  float shade_z = K_ONE - PowAbs(K_ONE - sat_z, C169.w);
  // Pair 2 (y, w channels) the same with an offset and its own power; each mixes into its pair-1 partner.
  float sat_y = clamp((depth_diff.y - C170.x) * C170.y, 0.0, 1.0);
  float sat_w = clamp((depth_diff.w - C170.x) * C170.y, 0.0, 1.0);
  float pow_y = PowAbs(K_ONE - sat_y, C170.z);
  float pow_w = PowAbs(K_ONE - sat_w, C170.z);
  float shadow_a = (shade_x * pow_y - shade_x) * C170.w + shade_x;
  float shadow_b = (shade_z * pow_w - shade_z) * C170.w + shade_z;
  float shadow_amount = max(shadow_a, shadow_b);
  float shadow_fade = K_ONE - (K_ONE - n_dot_l) * C169.y;                 // surfaces facing away from the light shadow less
  float lit = K_ONE - (C168.w * shadow_amount) * shadow_fade;             // 1 = fully lit

  // ---- 4. Light: sky / ground ambient plus the ambient cube, in the vertex's light colour ----
  vec3 ambient_cube = texture(samplerCube(t_ambient, s_ambient), normal).rgb * C197.xyz;
  ambient_cube = min(ambient_cube, C187.xyz);
  float up = dot(-normal, C180.xyz);
  float up_mix = K_HALF * up + K_HALF;                                      // 0 facing up, 1 facing down
  vec3 sky_ground = (C182.xyz - C181.xyz) * up_mix + C181.xyz;
  vec3 light = ambient_cube * lit + sky_ground * (C247.z * C189.xyz);
  vec3 base = ((albedo_tex * C190.xyz) * in_vertex.xyz) * light;            // the lit body colour

  // ---- 5. Highlight, rim and reflections ----
  // Highlight along the half vector between the normal and the view vector, strongest at grazing angles.
  vec3 half_sum = (normal + view) * K_HALF;
  vec3 half_dir = half_sum * InvLength(dot(half_sum, half_sum));
  float spec_dot = max(dot(half_dir, C12.xyz), K_EPS);
  float edge = K_ONE - abs(n_dot_v);                                         // 0 facing the camera, 1 at the silhouette
  float edge_pow = PowAbs(edge, C205.y);
  vec3 spec = (spec_dot * edge_pow) * C4.xyz * C205.x * in_light_pos.w;

  // The base plus the highlight, mixed by the two weights, and added back scaled by the rim strength: the rim is the
  // edge term times the arena's lighting map term, lifted by the specular mask.
  vec3 spec_mix = (base + spec) * C254.y - base + (base * (spec + K_ONE)) * C254.w;
  float lightmap_term = max(lightmap.y - in_light_pos.z, K_EPS) * C162.z + lightmap.x;
  float lightmap_pow = PowAbs(lightmap_term, C161.x);
  float spec_mask_mix = (normal1.w + K_MINUS_ONE) * C161.y + K_ONE;
  float rim_strength = (spec_mask_mix * (edge * lightmap_pow)) * C161.z;
  vec3 body = spec_mix * rim_strength + base;

  // Reflection cube along the reflected view vector (taken through the 3x3 in c52-c54): two channels, weighted and
  // masked by the specular mask.
  vec3 reflect_dir = vec3(dot(reflected, C52.xyz), dot(reflected, C53.xyz), dot(reflected, C54.xyz));
  vec2 reflect_xy = texture(samplerCube(t_reflect, s_reflect), reflect_dir).xy;
  float reflection = (reflect_xy.x * C197.w) * (normal1.w * C142.x) + (reflect_xy.y * C197.w) * (normal1.w * C142.z) + K_EPS;

  // The second cube map along the direction the vertex shader sends (its y flipped), masked by the mask texture.
  vec3 env_dir = vec3(in_env_dir.x, in_env_dir.y * K_MINUS_ONE, in_env_dir.z);
  vec3 env_tex = texture(samplerCube(t_env, s_env), env_dir).rgb * env_mask;

  // The sky seen at a glancing angle in the upward reflection: a rim colour, masked by the specular mask.
  float sky_rim = PowAbs(K_ONE + n_dot_v, C204.w);
  float sky_up = max((-reflected.y) * sky_rim, K_EPS);
  vec3 env = reflection * C144.xyz + ((sky_up * C204.xyz) * normal1.w + env_tex);
  vec3 env_lit = env * lit;

  // ---- 6. Sum, soft add of the reflections, fog ----
  vec3 color = body * (K_ONE - min(env_lit, K_ONE)) + env_lit;
  color = (C195.xyz - color) * in_uv_fog.z + color;
  XeOutputColor(vec4(color, in_vertex.w));
}
