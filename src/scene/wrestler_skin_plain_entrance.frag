#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md W2, the wrestlers' materials ours first): the wrestlers' plain body-skin material in
// ENTRANCES (vertex shader 3F91EC05, the yCh_skin family), in place of guest pixel shader 9F8F910305279A11, through the
// shader swap (src\scene_swap.h; SDK vulkan/native/shader_swap.h). The draw keeps its pass, textures, samplers and float
// constants; the colour goes out through the draw's own alpha test / exponent bias / gamma step (xenos_ps.glsl,
// kFlagOwnEpilogue).
//
// The same material as the entrance body-skin shader (wrestler_skin_entrance.frag, guest shader 91BF021CA7C0C1A5) without
// its paint parts, and written the same way, from the shader's data flow (the game's microcode read locally, checked
// against the SDK translator's own SPIR-V of it): plain multiplies (no Xenos 0 * inf rule, except the few places named
// Mul0 / PowAbs below, where the console's answer is kept on purpose), no texture sign switches (every texture is read
// unsigned, with no exponent adjust and no fetch-constant LOD bias; the shader's own LOD bias on the colour texture,
// c201.w, is kept), the ambient cube looked up by direction (the guest builds a face coordinate with its `cube`
// instruction and the translator turns it back into a direction, so the direction goes straight in), no rounding nudge on
// the coordinates, and implicit derivatives (the fetch inside the bump branch takes gradients worked out before it).
//
// What is gone compared with the entrance body-skin shader: the paint mask (the mask texture's x channel is not read),
// the paint colour tint (c246) and strength (c234), and the four taps of the mask around the coordinate. So the colour is
// the texture times the light colour and the vertex colour, the specular mask is mixed by the detail zone alone, and the
// bump branch (the tiled bump map, the second set of lights) is gated by the detail zone alone (c210.x times the zone).
// The compiler's literal bank is laid out differently: c254 holds the floor, the two highlight weights and the normal
// map's scale; c255 holds the plain numbers. As in the match variant (wrestler_skin_plain.frag) there is no reflection
// cube map; the reflections are four directional lights (a direction in c8-c11, a weight in the w of c0-c3), each raised
// to two lobe widths along the reflected view vector, with the second normal using its own single width (c210.y).
//
// What it computes, in order:
//   1. the colour texture, and the specular mask (mixed by the mask texture's other channels);
//   2. the surface normal: a basis made from the vertex normal and the vertex tangent, one normal map; and a second
//      normal (for the second set of lights) with a tiled bump map, only where the detail zone is on;
//   3. the shadow: the shadow map compared against the pixel's light-space depth, two filter pairs, faded with N.L,
//      plus the vertex's own shadow amount;
//   4. the light: the ambient cube map along the normal, tinted by the skin glow, plus a sky / ground ambient, then a
//      soft knee on bright light and a lift of the lit colour toward white;
//   5. a highlight along the half vector, a rim term scaled by the arena's lighting map, the four lights along the
//      reflected view vector (and again along the second normal's reflection where the bump weight is on);
//   6. the sum, a soft add of the lights, and the fog.
//
// Float constants (set 1 binding 2): the guest shader's registers in ascending order - 0, 1, 2, 3, 4, 8, 9, 10, 11, 12,
// 142, 144, 155, 156, 158, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 199, 201, 204, 205, 210,
// 218, 254, 255 (37; the translation's float_constants array is 37 too). 254 and 255 are the compiler's own literal bank
// (the guest shader loads them itself); what each holds is inferred from how it is used and is NOT captured, so they are
// read from the constants like any other (K_* below say what they appear to be).

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[37]; } fc;
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
#define C155 fc.c[12]  // xyz: glow colour at full strength; w: glow scale
#define C156 fc.c[13]  // x: knee strength (times the normal map's w); y: the lift's start level
#define C158 fc.c[14]  // xyz: ambient tint and glow colour at zero
#define C161 fc.c[15]  // x: lighting map power; y: specular-mask mix; z: rim strength
#define C162 fc.c[16]  // z: height-difference gain for the lighting map term
#define C166 fc.c[17]  // key light direction (for the shadow fade)
#define C168 fc.c[18]  // x, y: shadow depth range; z: depth bias; w: shadow strength
#define C169 fc.c[19]  // x unused; y: N.L fade; z: pair 1 gain; w: pair 1 power
#define C170 fc.c[20]  // x: pair 2 offset; y: pair 2 gain; z: pair 2 power; w: pair 2 mix
#define C180 fc.c[21]  // up axis
#define C181 fc.c[22]  // ambient colour at the top
#define C182 fc.c[23]  // ambient colour at the bottom
#define C187 fc.c[24]  // ceiling of the ambient cube's colour
#define C189 fc.c[25]  // weight of the sky / ground ambient
#define C190 fc.c[26]  // light colour (times the vertex colour)
#define C195 fc.c[27]  // fog colour
#define C197 fc.c[28]  // xyz: ambient cube colour scale
#define C199 fc.c[29]  // x: how much of the ambient cube's alpha drives the glow
#define C201 fc.c[30]  // w: LOD bias of the colour texture
#define C204 fc.c[31]  // xyz: rim colour; w: rim power
#define C205 fc.c[32]  // x: highlight gain; y: fresnel power; z, w: the two specular-mask levels mixed by the mask texture
#define C210 fc.c[33]  // x: how much the detail zone turns the bump branch on; y: lobe width (power) of the second normal's lights; z: tiling of the bump map
#define C218 fc.c[34]  // xyz: tangent / bitangent / normal weights of the perturbed normal
#define C254 fc.c[35]  // literal bank: x: small floor / epsilon (expected about 0); y, w: mix weights of the highlight; z: normal map's scale
#define C255 fc.c[36]  // literal bank: x expected 1, y expected 1.5 (only builds the guest's cube coordinate: not read here), z expected 0.5, w expected -1

#define K_EPS       C254.x  // floor of the clamps (about 0)
#define K_MIX_A     C254.y  // weight of the (base + highlight) term
#define K_NORMAL    C254.z  // scale of the normal map's offsets
#define K_MIX_B     C254.w  // weight of the (base * (highlight + 1)) term
#define K_ONE       C255.x
#define K_HALF      C255.z
#define K_MINUS_ONE C255.w

// Textures and samplers under the canonical set-3 bindings (1000 + fetch constant, 1300 + for cubes, 1200 + samplers).
layout(set = 3, binding = 1000) uniform texture2DArray t_albedo;      // fetch constant 0: colour
layout(set = 3, binding = 1001) uniform texture2DArray t_normal1;     // 1: normal map (x, y) and specular mask (w)
layout(set = 3, binding = 1004) uniform texture2DArray t_bump;        // 4: tiled bump map (x, y), read where the bump weight is on
layout(set = 3, binding = 1006) uniform texture2DArray t_zone;        // 6: detail zone (w)
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;      // 12: shadow map (four depths)
layout(set = 3, binding = 1014) uniform texture2DArray t_masks;       // 14: detail zone (y), specular level mix (z)
layout(set = 3, binding = 1015) uniform texture2DArray t_lightmap;    // 15: the arena's lighting map
layout(set = 3, binding = 1313) uniform textureCube t_ambient;        // 13: ambient (diffuse) cube; alpha drives the glow
layout(set = 3, binding = 1200) uniform sampler s_albedo;
layout(set = 3, binding = 1201) uniform sampler s_normal1;
layout(set = 3, binding = 1204) uniform sampler s_bump;
layout(set = 3, binding = 1206) uniform sampler s_zone;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1214) uniform sampler s_masks;
layout(set = 3, binding = 1215) uniform sampler s_lightmap;
layout(set = 3, binding = 1213) uniform sampler s_ambient;

// The guest's interpolators 0-7 and 9 (8 is not read).
layout(location = 0) in vec4 in_normal;       // xyz: vertex normal
layout(location = 1) in vec4 in_uv_fog;       // xy: texture coordinate; z: fog amount
layout(location = 2) in vec4 in_view;         // xyz: view vector (eye to surface)
layout(location = 3) in vec4 in_shadow_pos;   // xy / w: shadow map coordinate; z: light-space depth
layout(location = 4) in vec4 in_shadow_add;   // x: the vertex's own shadow amount, added to the map's
layout(location = 5) in vec4 in_light_pos;    // xy: lighting map coordinate; z: height; w: highlight scale
layout(location = 6) in vec4 in_tangent;      // xyz: tangent
layout(location = 7) in vec4 in_vertex;       // xyz: vertex light colour; w: alpha
layout(location = 9) in vec4 in_reflect_gain; // x: scale of the first lobe set

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

// The four lights' weighted sum for one lobe width: each light's log of its clamped dot, raised to the width (power of a
// log, with the console's 0 * infinity = 0 kept: a zero width on a zero dot is 1), times that light's weight, added from
// light 3 down to light 0.
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
  vec3 albedo_tex = texture(sampler2DArray(t_albedo, s_albedo), vec3(uv, 0.0), C201.w).rgb;  // read with a LOD bias
  vec4 normal1 = SAMPLE2D(t_normal1, s_normal1, uv);  // x, y: normal map; w: specular mask
  vec3 masks = SAMPLE2D(t_masks, s_masks, uv).xyz;    // y: detail zone; z: specular level mix (x is not used here)
  float zone_w = SAMPLE2D(t_zone, s_zone, uv).w;
  vec2 lightmap = SAMPLE2D(t_lightmap, s_lightmap, in_light_pos.xy).xy;

  float detail_zone = masks.y * zone_w;         // where the tiled bump applies and the specular mask is rebuilt
  float bump_weight = C210.x * detail_zone;     // above zero it turns on the second normal's own work

  // The specular mask: the normal map's w, pulled in the detail zone toward a level between c205.w and c205.z (chosen
  // by the mask texture's z).
  float spec_level = (C205.z - C205.w) * masks.z + C205.w;
  float spec_mask = detail_zone * (spec_level - normal1.w) + normal1.w;

  // The colour: the texture times the light colour and the vertex colour.
  vec3 diffuse = (albedo_tex * C190.xyz) * in_vertex.xyz;

  // ---- 2. The normal ----
  // The tangent frame from the vertex normal and tangent (bitangent = normal x tangent).
  vec3 tangent = in_tangent.xyz;
  vec3 bitangent = vec3(vertex_normal.y * tangent.z - vertex_normal.z * tangent.y,
                        vertex_normal.z * tangent.x - vertex_normal.x * tangent.z,
                        vertex_normal.x * tangent.y - vertex_normal.y * tangent.x);

  // Offsets from the normal map.
  float base_u = normal1.y * K_NORMAL + K_ONE;
  float base_v = normal1.x * K_NORMAL + K_ONE;
  vec3 normal = PerturbedNormal(tangent, bitangent, vertex_normal, base_u, base_v);

  // The normal the second set of lights uses: where the bump weight is on, the tiled bump map (weighted by the detail
  // zone) goes into the offsets; elsewhere it is the same normal.
  vec2 bump_uv = uv * C210.z;
  vec2 bump_dx = dFdxCoarse(bump_uv), bump_dy = dFdyCoarse(bump_uv);  // outside the branch: the fetch uses these as its gradients
  vec3 normal2 = normal;
  if (!(bump_weight <= 0.0)) {
    vec2 bump = textureGrad(sampler2DArray(t_bump, s_bump), vec3(bump_uv, 0.0), bump_dx, bump_dy).xy;
    float bump_v = (bump.x + bump.x) + K_MINUS_ONE;                   // unpacked to a signed value (y is flipped)
    float bump_u = (bump.y + bump.y) * K_MINUS_ONE + K_ONE;
    normal2 = PerturbedNormal(tangent, bitangent, vertex_normal, detail_zone * bump_u + base_u, detail_zone * bump_v + base_v);
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
  float lit = K_ONE - clamp((C168.w * shadow_amount) * shadow_fade + in_shadow_add.x, 0.0, 1.0);  // 1 = fully lit

  // ---- 4. Light: the ambient cube tinted by the skin glow, plus the sky / ground ambient ----
  vec4 ambient_tex = texture(samplerCube(t_ambient, s_ambient), normal);
  vec3 ambient_cube = min(ambient_tex.rgb * C197.xyz, C187.xyz);
  float up = dot(-normal, C180.xyz);
  float up_mix = K_HALF * up + K_HALF;                                      // 0 facing up, 1 facing down
  vec3 sky_ground = (C182.xyz - C181.xyz) * up_mix + C181.xyz;

  // The glow: the ambient cube's alpha (scaled, in shadow less) blends a colour from c158 to c155; the lit ambient is
  // tinted toward c158 in shadow, and the glow colour replaces part of it.
  float glow = (C199.x * ambient_tex.w) * lit;
  vec3 glow_color = ((C155.xyz - C158.xyz) * glow + C158.xyz) * C155.w;
  vec3 ambient_tint = (K_ONE - C158.xyz) * lit + C158.xyz;
  vec3 lit_ambient = (ambient_cube * lit) * ambient_tint * (K_ONE - glow_color) + glow_color * glow;
  vec3 light = sky_ground * C189.xyz + lit_ambient;

  // A soft knee on bright light: the part of the light above 1 is cut by (1 - knee); the knee's strength comes from the
  // normal map's w. The lit colour is then lifted toward white where the light is above c156.y.
  float knee = min(C156.x * normal1.w, K_ONE);
  vec3 light_low = min(light, K_ONE);
  vec3 light_knee = (light - light_low) * (K_ONE - knee) + light_low;
  vec3 lift = clamp((light - C156.y) * knee, 0.0, 1.0);
  vec3 lit_color = diffuse * light_knee;
  vec3 base = lift * (K_ONE - lit_color) + lit_color;                       // the lit body colour

  // ---- 5. Highlight, rim and the four lights ----
  // Highlight along the half vector between the normal and the view vector, strongest at grazing angles.
  vec3 half_sum = (normal + view) * K_HALF;
  vec3 half_dir = half_sum * InvLength(dot(half_sum, half_sum));
  float spec_dot = max(dot(half_dir, C12.xyz), K_EPS);
  float edge = K_ONE - abs(n_dot_v);                                         // 0 facing the camera, 1 at the silhouette
  float edge_pow = PowAbs(edge, C205.y);
  vec3 spec = (spec_dot * edge_pow) * C4.xyz * C205.x * in_light_pos.w;

  // The base plus the highlight, mixed by the two weights, and added back scaled by the rim strength: the rim is the
  // edge term times the arena's lighting map term, lifted by the normal map's w.
  vec3 spec_mix = (base + spec) * K_MIX_A - base + (base * (spec + K_ONE)) * K_MIX_B;
  float lightmap_term = max(lightmap.y - in_light_pos.z, K_EPS) * C162.z + lightmap.x;
  float lightmap_pow = PowAbs(lightmap_term, C161.x);
  float spec_mask_mix = (normal1.w - K_ONE) * C161.y + K_ONE;
  float rim_strength = (spec_mask_mix * (edge * lightmap_pow)) * C161.z;
  vec3 body = spec_mix * rim_strength + base;

  // The four lights along the reflected view vector: each clamped dot, its log, then two lobe widths (c142.y and
  // c142.w), weighted by the specular mask and the two lobe weights (c142.x and c142.z); the first lobe set is also
  // scaled by interpolator 9.
  vec4 light_dots = vec4(dot(reflected, C8.xyz), dot(reflected, C9.xyz), dot(reflected, C10.xyz), dot(reflected, C11.xyz));
  vec4 light_logs = log2(abs(max(light_dots, K_EPS)));
  float lobes_a = WeightedLobes(light_logs, C142.y);
  float lobes_b = WeightedLobes(light_logs, C142.w);
  float spec_light = (spec_mask * C142.x) * lobes_a * in_reflect_gain.x + (spec_mask * C142.z) * lobes_b;

  // Where the bump weight is on, the second normal's own light: the same four lights along its reflection, one lobe
  // width (c210.y), scaled by the bump weight. Zero elsewhere.
  float bump_light = 0.0;
  if (bump_weight > 0.0) {
    vec4 bump_dots = vec4(dot(reflected2, C8.xyz), dot(reflected2, C9.xyz), dot(reflected2, C10.xyz), dot(reflected2, C11.xyz));
    vec4 bump_logs = log2(abs(max(bump_dots, K_EPS)));
    bump_light = bump_weight * WeightedLobes(bump_logs, C210.y);
  }
  float light_total = spec_light + bump_light;

  // The sky seen at a glancing angle in the upward reflection: a rim colour, masked by the normal map's w.
  float sky_rim = PowAbs(K_ONE + n_dot_v, C204.w);
  float sky_up = max((-reflected.y) * sky_rim, K_EPS);
  vec3 env = light_total * C144.xyz + (sky_up * C204.xyz) * normal1.w;
  vec3 env_lit = env * lit;

  // ---- 6. Sum, soft add of the lights, fog ----
  vec3 color = body * (K_ONE - min(env_lit, K_ONE)) + env_lit;
  color = (C195.xyz - color) * in_uv_fog.z + color;
  XeOutputColor(vec4(color, in_vertex.w));
}
