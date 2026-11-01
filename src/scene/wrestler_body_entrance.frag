#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md W2, the wrestlers' body material ours first): the wrestlers' body / gear material in
// ENTRANCES, in place of guest pixel shader B1F58C0AA0E3C83A, through the shader swap (src\scene_swap.h; SDK
// vulkan/native/shader_swap.h). The draw keeps its pass, textures, samplers and float constants; the colour goes out
// through the draw's own alpha test / exponent bias / gamma step (xenos_ps.glsl, kFlagOwnEpilogue).
//
// The same material family as the match variant (wrestler_body.frag, guest shader 1B66FF87A6433D53) and written the
// same way, from the shader's data flow (the game's microcode read locally, checked against the SDK translator's own
// SPIR-V of it): plain multiplies (no Xenos 0 * inf rule, except the few places named Mul0 / PowAbs / InvLength below,
// where the console's answer is kept on purpose), no texture sign switches (every texture is read unsigned, with no
// exponent adjust and no fetch-constant LOD bias), the ambient cube looked up by direction (the guest builds a face
// coordinate with its `cube` instruction and the translator turns it back into a direction, so the direction goes
// straight in), no rounding nudge on the coordinates, and implicit derivatives (the four mask taps inside a branch take
// gradients worked out before it).
//
// What differs from the match variant: there is no reflection cube map and no 3x3 matrix for it (c52-c54 are gone);
// instead the reflections are four directional lights (a direction in c8-c11, a weight in the w of c0-c3), each raised
// to two lobe widths along the reflected view vector, with the paint area using its own single width (c210.y). The
// guest's literal bank (c252-c255) is laid out differently (see K_* below).
//
// What it computes, in order:
//   1. the colour texture tinted by the paint mask, and a detail texture that lightens / darkens it;
//   2. the surface normal: a basis made from the vertex normal and the up axis, two normal maps, and - where the
//      paint mask is on - the mask's own bump (four taps of the mask texture);
//   3. the shadow: the shadow map compared against the pixel's light-space depth, two filter pairs, faded with N.L;
//   4. the light: a sky / ground ambient by the up axis plus the ambient cube map along the normal, times the shadow;
//   5. a highlight along the half vector, a rim term scaled by the arena's lighting map, the four lights along the
//      reflected view vector (and again along the paint bump's reflection where the paint mask is on);
//   6. the sum, a soft add of the lights, and the fog.
//
// Float constants (set 1 binding 2): the guest shader's registers in ascending order - 0, 1, 2, 3, 4, 8, 9, 10, 11,
// 12, 142, 144, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 204, 205, 206, 208, 210, 218,
// 234, 246, 252, 253, 254, 255 (38; registered as kWrestlerBodyEntranceFloats). 252-255 are the compiler's own literal
// bank (the guest shader loads them itself); what each holds is inferred from how it is used and is NOT captured, so
// they are read from the constants like any other (K_* below say what they appear to be).

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[38]; } fc;
#define C0   fc.c[0]   // w: weight of light 0
#define C1   fc.c[1]   // w: weight of light 1
#define C2   fc.c[2]   // w: weight of light 2
#define C3   fc.c[3]   // w: weight of light 3 (xyz of c0-c3 are not read by this shader)
#define C4   fc.c[4]   // specular colour
#define C8   fc.c[5]   // direction of light 0
#define C9   fc.c[6]   // direction of light 1
#define C10  fc.c[7]   // direction of light 2
#define C11  fc.c[8]   // direction of light 3
#define C12  fc.c[9]   // direction the highlight is measured against
#define C142 fc.c[10]  // x, z: the two lobe weights (times the specular mask); y, w: the two lobe widths (powers)
#define C144 fc.c[11]  // colour the light amount adds
#define C161 fc.c[12]  // x: lighting map power; y: specular-mask mix; z: rim strength
#define C162 fc.c[13]  // z: height-difference gain for the lighting map term
#define C166 fc.c[14]  // key light direction (for the shadow fade)
#define C168 fc.c[15]  // x, y: shadow depth range; z: depth bias; w: shadow strength
#define C169 fc.c[16]  // x unused; y: N.L fade; z: pair 1 gain; w: pair 1 power
#define C170 fc.c[17]  // x: pair 2 offset; y: pair 2 gain; z: pair 2 power; w: pair 2 mix
#define C180 fc.c[18]  // up axis
#define C181 fc.c[19]  // ambient colour at the top
#define C182 fc.c[20]  // ambient colour at the bottom
#define C187 fc.c[21]  // ceiling of the ambient cube's colour
#define C189 fc.c[22]  // weight of the sky / ground ambient
#define C190 fc.c[23]  // light colour (times the vertex colour)
#define C195 fc.c[24]  // fog colour
#define C197 fc.c[25]  // xyz: ambient cube colour scale
#define C204 fc.c[26]  // xyz: rim colour; w: rim power
#define C205 fc.c[27]  // x: highlight gain; y: fresnel power
#define C206 fc.c[28]  // material set A (x: detail tiling; y: normal gain; z: detail gain; w: light detail gain)
#define C208 fc.c[29]  // material set B (same layout, y z w used), mixed in by the vertex's blend value
#define C210 fc.c[30]  // y: lobe width (power) of the paint area's own light
#define C218 fc.c[31]  // xyz: tangent / bitangent / normal weights of the perturbed normal
#define C234 fc.c[32]  // x: paint mask strength
#define C246 fc.c[33]  // xyz: paint colour
#define C252 fc.c[34]  // x: small floor / epsilon (expected about 0)
#define C253 fc.c[35]  // x: expected 2 (map unpack scale); y, w: mix weights of the highlight; z: first normal map's scale
#define C254 fc.c[36]  // literal bank: x expected -1, y expected 1, z expected 0.5, w expected 1.5 (only builds the guest's cube coordinate: not read here)
#define C255 fc.c[37]  // x, w: tap offsets in u; y, z: tap offsets in v (the mask's four taps)

#define K_EPS       C252.x  // floor of the clamps (about 0)
#define K_TWO       C253.x  // scale of an unpacked map value
#define K_MINUS_ONE C254.x
#define K_ONE       C254.y
#define K_HALF      C254.z

// Textures and samplers under the canonical set-3 bindings (1000 + fetch constant, 1300 + for cubes, 1200 + samplers).
layout(set = 3, binding = 1000) uniform texture2DArray t_albedo;      // fetch constant 0: colour
layout(set = 3, binding = 1001) uniform texture2DArray t_normal1;     // 1: first normal map (x, y) and specular mask (w)
layout(set = 3, binding = 1003) uniform texture2DArray t_detail;      // 3: tiled detail: normal (x, y), detail light / dark (w)
layout(set = 3, binding = 1006) uniform texture2DArray t_detail_mask; // 6: where the detail applies (z)
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;      // 12: shadow map (four depths)
layout(set = 3, binding = 1014) uniform texture2DArray t_paint_mask;  // 14: paint mask
layout(set = 3, binding = 1015) uniform texture2DArray t_lightmap;    // 15: the arena's lighting map
layout(set = 3, binding = 1313) uniform textureCube t_ambient;        // 13: ambient (diffuse) cube
layout(set = 3, binding = 1200) uniform sampler s_albedo;
layout(set = 3, binding = 1201) uniform sampler s_normal1;
layout(set = 3, binding = 1203) uniform sampler s_detail;
layout(set = 3, binding = 1206) uniform sampler s_detail_mask;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1214) uniform sampler s_paint_mask;
layout(set = 3, binding = 1215) uniform sampler s_lightmap;
layout(set = 3, binding = 1213) uniform sampler s_ambient;

// The guest's interpolators 0-4, 6 and 8 (5, 7 and 9 are not read).
layout(location = 0) in vec4 in_normal;     // xyz: vertex normal
layout(location = 1) in vec4 in_uv_fog;     // xy: texture coordinate; z: fog amount
layout(location = 2) in vec4 in_view;       // xyz: view vector (eye to surface)
layout(location = 3) in vec4 in_blend;     // x: blend between material sets A and B
layout(location = 4) in vec4 in_shadow_pos; // xy / w: shadow map coordinate; z: light-space depth
layout(location = 6) in vec4 in_light_pos;  // xy: lighting map coordinate; z: height; w: highlight scale
layout(location = 8) in vec4 in_vertex;     // xyz: vertex light colour; w: alpha

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

// The four lights' weighted sum for one lobe width: each light's log of its clamped dot (taken once by the caller), raised
// to the width (power of a log, with the console's 0 * infinity = 0 kept: a zero width on a zero dot is 1), times that
// light's weight, added from light 3 down to light 0.
float WeightedLobes(vec4 light_logs, float width) {
  vec4 lobe = vec4(exp2(Mul0(light_logs.x, width)), exp2(Mul0(light_logs.y, width)),
                   exp2(Mul0(light_logs.z, width)), exp2(Mul0(light_logs.w, width)));
  float sum = lobe.w * C3.w;
  sum = lobe.z * C2.w + sum;
  sum = lobe.y * C1.w + sum;
  return lobe.x * C0.w + sum;
}

void main() {
  vec2 uv = in_uv_fog.xy;
  vec3 vertex_normal = in_normal.xyz;
  vec3 view = in_view.xyz;

  // ---- 1. Texture reads at the model coordinate ----
  vec3 albedo_tex = SAMPLE2D(t_albedo, s_albedo, uv).rgb;
  float detail_mask = SAMPLE2D(t_detail_mask, s_detail_mask, uv).z;
  float paint_mask = SAMPLE2D(t_paint_mask, s_paint_mask, uv).x;
  vec4 normal1 = SAMPLE2D(t_normal1, s_normal1, uv);  // x, y: normal map; w: specular mask
  vec4 detail = SAMPLE2D(t_detail, s_detail, uv * C206.x);
  vec2 lightmap = SAMPLE2D(t_lightmap, s_lightmap, in_light_pos.xy).xy;

  // Material parameters, mixed per vertex between set A and set B.
  float blend = in_blend.x;
  float detail_gain = (C208.z - C206.z) * blend + C206.z;
  float normal_gain = (C208.y - C206.y) * blend + C206.y;
  float light_detail_gain = (C208.w - C206.w) * blend + C206.w;

  // The paint mask: its strength (squared, clamped to 1) and how much of the detail survives under it.
  float paint_sq = (C234.x * paint_mask) * paint_mask;
  float detail_keep = K_ONE - min(paint_sq, K_ONE);

  // The colour: tinted by the paint colour where the mask is on, then lightened / darkened by the detail texture
  // (its w channel, unpacked to a signed value) where the detail mask allows.
  vec3 tint = (C246.xyz + K_MINUS_ONE) * paint_mask + K_ONE;
  vec3 tinted = tint * albedo_tex;
  float detail_signed = detail.w * K_TWO + K_MINUS_ONE;
  vec3 diffuse = ((tinted * detail_signed) * detail_gain) * detail_mask + tinted;

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

  // Offsets from the first normal map and from the (tiled) detail normal map, which is weakened under the paint mask.
  float base_u = normal1.y * C253.z + K_ONE;
  float base_v = normal1.x * C253.z + K_ONE;
  float detail_nx = detail.x * K_TWO + K_MINUS_ONE;
  float detail_ny = detail.y * K_TWO + K_MINUS_ONE;
  float detail_u = ((detail_ny * normal_gain) * detail_keep) * detail_mask;
  float detail_v = ((detail_nx * normal_gain) * detail_keep) * detail_mask;
  vec3 normal = PerturbedNormal(tangent, bitangent, vertex_normal, base_u + detail_u, base_v + detail_v);

  // The normal the paint area's light uses: where the paint mask is on, the mask's own bump (four taps of the mask
  // texture around the coordinate) goes into the offsets; elsewhere it is the same normal.
  vec2 uv_dx = dFdxCoarse(uv), uv_dy = dFdyCoarse(uv);  // outside the branch: the taps use these as their gradients
  vec3 normal2 = normal;
  if (!(paint_sq <= 0.0)) {
    float tap0 = textureGrad(sampler2DArray(t_paint_mask, s_paint_mask), vec3(uv.x + C255.x, uv.y + C255.y, 0.0), uv_dx, uv_dy).x;
    float tap1 = textureGrad(sampler2DArray(t_paint_mask, s_paint_mask), vec3(uv.x + C255.x, uv.y + C255.z, 0.0), uv_dx, uv_dy).x;
    float tap2 = textureGrad(sampler2DArray(t_paint_mask, s_paint_mask), vec3(uv.x + C255.w, uv.y + C255.z, 0.0), uv_dx, uv_dy).x;
    float tap3 = textureGrad(sampler2DArray(t_paint_mask, s_paint_mask), vec3(uv.x + C255.w, uv.y + C255.y, 0.0), uv_dx, uv_dy).x;
    float d0 = paint_mask - tap0, d1 = paint_mask - tap1, d2 = paint_mask - tap2, d3 = paint_mask - tap3;
    float grad_u = (d2 * K_MINUS_ONE + d1 * K_MINUS_ONE + d0) + d3;                          // along the bitangent side
    float grad_v = d3 * K_MINUS_ONE + d2 * K_MINUS_ONE + d1 * K_ONE + d0 * K_ONE;             // along the tangent side
    float u2 = (detail_u - paint_mask * grad_u) + base_u;
    float v2 = (detail_v - paint_mask * grad_v) + base_v;
    normal2 = PerturbedNormal(tangent, bitangent, vertex_normal, u2, v2);
  }

  // ---- Reflection vectors ----
  float n_dot_v = dot(normal, view);
  vec3 reflected = view - normal * (n_dot_v + n_dot_v);
  float n2_dot_v = dot(normal2, view);
  vec3 reflected2 = view - normal2 * (n2_dot_v + n2_dot_v);

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
  vec3 light = sky_ground * C189.xyz + ambient_cube * lit;
  vec3 base = ((C190.xyz * in_vertex.xyz) * diffuse) * light;               // the lit body colour

  // ---- 5. Highlight, rim and the four lights ----
  // Highlight along the half vector between the normal and the view vector, strongest at grazing angles.
  vec3 half_sum = (normal + view) * K_HALF;
  vec3 half_dir = half_sum * InvLength(dot(half_sum, half_sum));
  float spec_dot = max(dot(half_dir, C12.xyz), K_EPS);
  float edge = K_ONE - abs(n_dot_v);                                         // 0 facing the camera, 1 at the silhouette
  float edge_pow = PowAbs(edge, C205.y);
  vec3 spec = (spec_dot * edge_pow) * C4.xyz * C205.x * in_light_pos.w;

  // The base plus the highlight, mixed by the two weights, and added back scaled by the rim strength: the rim is the
  // edge term times the arena's lighting map term, lifted by the specular mask.
  vec3 spec_mix = (base + spec) * C253.y - base + (base * (spec + K_ONE)) * C253.w;
  float lightmap_term = max(lightmap.y - in_light_pos.z, K_EPS) * C162.z + lightmap.x;
  float lightmap_pow = PowAbs(lightmap_term, C161.x);
  float spec_mask_mix = (normal1.w - K_ONE) * C161.y + K_ONE;
  float rim_strength = (spec_mask_mix * (edge * lightmap_pow)) * C161.z;
  vec3 body = spec_mix * rim_strength + base;

  // The four lights along the reflected view vector: each clamped dot, its log, then two lobe widths (c142.y and
  // c142.w), weighted by the specular mask and the two lobe weights (c142.x and c142.z).
  vec4 light_dots = vec4(dot(reflected, C8.xyz), dot(reflected, C9.xyz), dot(reflected, C10.xyz), dot(reflected, C11.xyz));
  vec4 light_logs = log2(abs(max(light_dots, K_EPS)));
  float lobes_a = WeightedLobes(light_logs, C142.y);
  float lobes_b = WeightedLobes(light_logs, C142.w);
  float spec_light = (normal1.w * C142.x) * lobes_a + (normal1.w * C142.z) * lobes_b + K_EPS;

  // Where the paint mask is on, the paint area's own light: the same four lights along the paint bump's reflection,
  // one lobe width (c210.y), scaled by the paint strength. Zero elsewhere.
  float paint_light = 0.0;
  if (paint_sq > 0.0) {
    vec4 paint_dots = vec4(dot(reflected2, C8.xyz), dot(reflected2, C9.xyz), dot(reflected2, C10.xyz), dot(reflected2, C11.xyz));
    vec4 paint_logs = log2(abs(max(paint_dots, K_EPS)));
    paint_light = paint_sq * WeightedLobes(paint_logs, C210.y);
  }
  float light_total = spec_light + paint_light;

  // The sky seen at a glancing angle in the upward reflection: a rim colour, masked by the specular mask.
  float sky_rim = PowAbs(K_ONE + n_dot_v, C204.w);
  float sky_up = max((-reflected.y) * sky_rim, K_EPS);
  vec3 env = light_total * C144.xyz + (sky_up * C204.xyz) * normal1.w;
  // Detail lightens / darkens the added light the way it does the colour, less under the paint mask.
  vec3 env_detail = (((env * detail_signed) * light_detail_gain) * detail_keep) * detail_mask + env;
  vec3 env_lit = env_detail * lit;

  // ---- 6. Sum, soft add of the lights, fog ----
  vec3 color = body * (K_ONE - min(env_lit, K_ONE)) + env_lit;
  color = (C195.xyz - color) * in_uv_fog.z + color;
  XeOutputColor(vec4(color, in_vertex.w));
}
