#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md W2, the wrestlers' body material ours first): the wrestlers' body-skin material in
// matches, in place of guest pixel shader B08B5E734D48F5D0 (vertex shader 3F91EC05), through the shader swap
// (src\scene_swap.h; SDK vulkan/native/shader_swap.h). The draw keeps its pass, textures, samplers and float constants;
// the colour goes out through the draw's own alpha test / exponent bias / gamma step (xenos_ps.glsl, kFlagOwnEpilogue).
//
// The same material family as the gear shader (wrestler_body.frag) and written the same way, from the shader's data
// flow (the game's microcode read locally, checked against the SDK translator's own SPIR-V of it): plain multiplies (no
// Xenos 0 * inf rule; the reciprocal roots are guarded instead, see InvLength), no texture sign switches (every
// texture is read unsigned, with no exponent adjust and no fetch-constant LOD bias), cube lookups by direction (the
// guest builds a face coordinate with its `cube` instruction and the translator turns it back into a direction, so the
// direction goes straight in), no rounding nudge on the coordinates, and implicit derivatives (the fetches inside a
// branch take gradients worked out before it).
//
// What differs from the gear shader: the tangent comes from the vertex (interpolator 6) instead of being built from the
// up axis; there is no tiled detail / mask pair for the colour (the albedo is read with a LOD bias held in c201.w); the
// tiled bump map (fetch constant 4) only shapes the second normal; a skin "glow" term (c155 / c156 / c158 / c199) tints
// the ambient light and brightens the lit colour; the specular mask is mixed from two sources; and the shadow gets a
// per-vertex amount (interpolator 4) added to it.
//
// What it computes, in order:
//   1. the colour texture tinted by the paint mask, and the specular mask (mixed by the mask texture's other channels);
//   2. the surface normal: a basis made from the vertex normal and the vertex tangent, one normal map; and a second
//      normal (for the paint area's reflection) with a tiled bump map and the paint mask's own bump (four taps);
//   3. the shadow: the shadow map compared against the pixel's light-space depth, two filter pairs, faded with N.L,
//      plus the vertex's own shadow amount;
//   4. the light: the ambient cube map along the normal, tinted by the skin glow, plus a sky / ground ambient, then a
//      soft knee on bright light and a lift of the lit colour toward white;
//   5. a highlight along the half vector, a rim term scaled by the arena's lighting map, a reflection cube map along
//      the reflected view vector, and the paint area's own reflection;
//   6. the sum, a soft add of the reflections, and the fog.
//
// Float constants (set 1 binding 2): the guest shader's registers in ascending order - 4, 12, 52, 53, 54, 142, 144,
// 155, 156, 158, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 199, 201, 204, 205, 210, 218,
// 234, 246, 253, 254, 255 (35; the translation's float_constants array is 35 too). 253-255 are the compiler's own
// literal bank (the guest shader loads them itself); what each holds is inferred from how it is used and is NOT
// captured, so they are read from the constants like any other (K_* below say what they appear to be).

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[35]; } fc;
#define C4   fc.c[0]   // specular colour
#define C12  fc.c[1]   // direction the highlight is measured against
#define C52  fc.c[2]   // rows of the matrix that takes the reflected vector into the reflection cube's space
#define C53  fc.c[3]
#define C54  fc.c[4]
#define C142 fc.c[5]   // x, z: the reflection cube's two channel weights
#define C144 fc.c[6]   // colour the reflection amount adds
#define C155 fc.c[7]   // xyz: glow colour at full strength; w: glow scale
#define C156 fc.c[8]   // x: knee strength (times the normal map's w); y: the lift's start level
#define C158 fc.c[9]   // xyz: ambient tint and glow colour at zero
#define C161 fc.c[10]  // x: lighting map power; y: specular-mask mix; z: rim strength
#define C162 fc.c[11]  // z: height-difference gain for the lighting map term
#define C166 fc.c[12]  // key light direction (for the shadow fade)
#define C168 fc.c[13]  // x, y: shadow depth range; z: depth bias; w: shadow strength
#define C169 fc.c[14]  // x unused; y: N.L fade; z: pair 1 gain; w: pair 1 power
#define C170 fc.c[15]  // x: pair 2 offset; y: pair 2 gain; z: pair 2 power; w: pair 2 mix
#define C180 fc.c[16]  // up axis
#define C181 fc.c[17]  // ambient colour at the top
#define C182 fc.c[18]  // ambient colour at the bottom
#define C187 fc.c[19]  // ceiling of the ambient cube's colour
#define C189 fc.c[20]  // weight of the sky / ground ambient
#define C190 fc.c[21]  // light colour (times the vertex colour)
#define C195 fc.c[22]  // fog colour
#define C197 fc.c[23]  // xyz: ambient cube colour scale; w: reflection cube scale
#define C199 fc.c[24]  // x: how much of the ambient cube's alpha drives the glow
#define C201 fc.c[25]  // w: LOD bias of the colour texture
#define C204 fc.c[26]  // xyz: rim colour; w: rim power
#define C205 fc.c[27]  // x: highlight gain; y: fresnel power; z, w: the two specular-mask levels mixed by the mask texture
#define C210 fc.c[28]  // x: how much the detail zone adds to the bump weight; z: tiling of the bump map
#define C218 fc.c[29]  // xyz: tangent / bitangent / normal weights of the perturbed normal
#define C234 fc.c[30]  // x: paint mask strength
#define C246 fc.c[31]  // xyz: paint colour
#define C253 fc.c[32]  // x: small floor / epsilon (expected about 0); y, w: mix weights of the highlight; z: normal map's scale
#define C254 fc.c[33]  // literal bank: x expected 1, y expected 1.5 (only builds the guest's cube coordinate: not read here), z expected 0.5, w expected -1
#define C255 fc.c[34]  // x, w: tap offsets in u; y, z: tap offsets in v (the mask's four taps)

#define K_EPS       C253.x  // floor of the clamps (about 0)
#define K_ONE       C254.x
#define K_HALF      C254.z
#define K_MINUS_ONE C254.w

// Textures and samplers under the canonical set-3 bindings (1000 + fetch constant, 1300 + for cubes, 1200 + samplers).
layout(set = 3, binding = 1000) uniform texture2DArray t_albedo;      // fetch constant 0: colour
layout(set = 3, binding = 1001) uniform texture2DArray t_normal1;     // 1: normal map (x, y) and specular mask (w)
layout(set = 3, binding = 1004) uniform texture2DArray t_bump;        // 4: tiled bump map (x, y), read where the bump weight is on
layout(set = 3, binding = 1006) uniform texture2DArray t_zone;        // 6: detail zone (w)
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;      // 12: shadow map (four depths)
layout(set = 3, binding = 1014) uniform texture2DArray t_masks;       // 14: paint mask (x), detail zone (y), specular level mix (z)
layout(set = 3, binding = 1015) uniform texture2DArray t_lightmap;    // 15: the arena's lighting map
layout(set = 3, binding = 1311) uniform textureCube t_reflect;        // 11: reflection cube
layout(set = 3, binding = 1313) uniform textureCube t_ambient;        // 13: ambient (diffuse) cube; alpha drives the glow
layout(set = 3, binding = 1200) uniform sampler s_albedo;
layout(set = 3, binding = 1201) uniform sampler s_normal1;
layout(set = 3, binding = 1204) uniform sampler s_bump;
layout(set = 3, binding = 1206) uniform sampler s_zone;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1214) uniform sampler s_masks;
layout(set = 3, binding = 1215) uniform sampler s_lightmap;
layout(set = 3, binding = 1211) uniform sampler s_reflect;
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
layout(location = 9) in vec4 in_reflect_gain; // x: scale of the first reflection channel

#define SAMPLE2D(tex, smp, uv) texture(sampler2DArray(tex, smp), vec3((uv), 0.0))

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
  vec3 albedo_tex = texture(sampler2DArray(t_albedo, s_albedo), vec3(uv, 0.0), C201.w).rgb;  // read with a LOD bias
  vec4 normal1 = SAMPLE2D(t_normal1, s_normal1, uv);  // x, y: normal map; w: specular mask
  vec3 masks = SAMPLE2D(t_masks, s_masks, uv).xyz;    // x: paint mask; y: detail zone; z: specular level mix
  float zone_w = SAMPLE2D(t_zone, s_zone, uv).w;
  vec2 lightmap = SAMPLE2D(t_lightmap, s_lightmap, in_light_pos.xy).xy;

  float paint_mask = masks.x;
  float detail_zone = masks.y * zone_w;         // where the tiled bump applies and the specular mask is rebuilt
  float zone_weight = C210.x * detail_zone;

  // The bump weight: the paint mask squared (times its strength), plus the detail zone's weight where it is not
  // painted. Above zero it turns on the second normal's own work (the bump taps and the second reflection).
  float bump_weight = (paint_mask * C234.x - zone_weight) * paint_mask + zone_weight;

  // The specular mask: the normal map's w, pulled in the detail zone toward a level between c205.w and c205.z (chosen
  // by the mask texture's z) that fades out under the paint.
  float spec_level = (C205.z - C205.w) * masks.z + C205.w;
  float spec_mask = detail_zone * (spec_level * (K_ONE - paint_mask) - normal1.w) + normal1.w;

  // The colour: tinted by the paint colour where the mask is on, times the light colour and the vertex colour.
  vec3 tint = (C246.xyz + K_MINUS_ONE) * paint_mask + K_ONE;
  vec3 diffuse = (tint * (albedo_tex * C190.xyz)) * in_vertex.xyz;

  // ---- 2. The normal ----
  // The tangent frame from the vertex normal and tangent (bitangent = normal x tangent).
  vec3 tangent = in_tangent.xyz;
  vec3 bitangent = vec3(vertex_normal.y * tangent.z - vertex_normal.z * tangent.y,
                        vertex_normal.z * tangent.x - vertex_normal.x * tangent.z,
                        vertex_normal.x * tangent.y - vertex_normal.y * tangent.x);

  // Offsets from the normal map.
  float base_u = normal1.y * C253.z + K_ONE;
  float base_v = normal1.x * C253.z + K_ONE;
  vec3 normal = PerturbedNormal(tangent, bitangent, vertex_normal, base_u, base_v);

  // The normal the paint area's reflection uses: where the bump weight is on, the tiled bump map (weighted by the
  // detail zone) and the paint mask's own bump (four taps of the mask texture around the coordinate) go into the
  // offsets; elsewhere it is the same normal.
  vec2 uv_dx = dFdxCoarse(uv), uv_dy = dFdyCoarse(uv);  // outside the branch: the fetches use these as their gradients
  vec2 bump_uv = uv * C210.z;
  vec2 bump_dx = dFdxCoarse(bump_uv), bump_dy = dFdyCoarse(bump_uv);
  vec3 normal2 = normal;
  if (!(bump_weight <= 0.0)) {
    float tap0 = textureGrad(sampler2DArray(t_masks, s_masks), vec3(uv.x + C255.x, uv.y + C255.y, 0.0), uv_dx, uv_dy).x;
    float tap1 = textureGrad(sampler2DArray(t_masks, s_masks), vec3(uv.x + C255.x, uv.y + C255.z, 0.0), uv_dx, uv_dy).x;
    float tap2 = textureGrad(sampler2DArray(t_masks, s_masks), vec3(uv.x + C255.w, uv.y + C255.z, 0.0), uv_dx, uv_dy).x;
    float tap3 = textureGrad(sampler2DArray(t_masks, s_masks), vec3(uv.x + C255.w, uv.y + C255.y, 0.0), uv_dx, uv_dy).x;
    vec2 bump = textureGrad(sampler2DArray(t_bump, s_bump), vec3(bump_uv, 0.0), bump_dx, bump_dy).xy;
    float bump_v = (bump.x + bump.x) * K_ONE + K_MINUS_ONE;           // unpacked to a signed value (y is flipped)
    float bump_u = (bump.y + bump.y) * K_MINUS_ONE + K_ONE;
    float u1 = detail_zone * bump_u + base_u;
    float v1 = detail_zone * bump_v + base_v;
    float d0 = paint_mask - tap0, d1 = paint_mask - tap1, d2 = paint_mask - tap2, d3 = paint_mask - tap3;
    float grad_u = (d2 * K_MINUS_ONE + d1 * K_MINUS_ONE + d0) + d3;                          // along the bitangent side
    float grad_v = d3 * K_MINUS_ONE + d2 * K_MINUS_ONE + d1 * K_ONE + d0 * K_ONE;             // along the tangent side
    float u2 = u1 + grad_u * (-paint_mask);
    float v2 = v1 + grad_v * (-paint_mask);
    normal2 = PerturbedNormal(tangent, bitangent, vertex_normal, u2, v2);
  }

  // ---- Reflection vectors ----
  float n_dot_v = dot(normal, view);
  vec3 reflected = view - normal * (n_dot_v + n_dot_v);
  float n2_dot_v = dot(normal2, view);
  vec3 reflected2 = view - normal2 * (n2_dot_v + n2_dot_v);

  // ---- 3. Shadow ----
  float n_dot_l = max(dot(normal, C166.xyz), K_EPS);
  vec2 shadow_uv = in_shadow_pos.xy * (1.0 / in_shadow_pos.w);
  vec4 occluder = textureLod(sampler2DArray(t_shadow, s_shadow), vec3(shadow_uv, 0.0), 0.0);
  float receiver = clamp((in_shadow_pos.z - C168.x) * (1.0 / (C168.y - C168.x)) - C168.z, 0.0, 1.0);
  vec4 depth_diff = vec4(receiver) - occluder;
  // Pair 1 (x, z channels): saturated difference, flipped, raised to a power, flipped back.
  float sat_x = clamp(depth_diff.x * C169.z, 0.0, 1.0);
  float sat_z = clamp(depth_diff.z * C169.z, 0.0, 1.0);
  float shade_x = K_ONE - exp2(C169.w * log2(abs(K_ONE - sat_x)));
  float shade_z = K_ONE - exp2(C169.w * log2(abs(K_ONE - sat_z)));
  // Pair 2 (y, w channels) the same with an offset and its own power; each mixes into its pair-1 partner.
  float sat_y = clamp((depth_diff.y - C170.x) * C170.y, 0.0, 1.0);
  float sat_w = clamp((depth_diff.w - C170.x) * C170.y, 0.0, 1.0);
  float pow_y = exp2(C170.z * log2(abs(K_ONE - sat_y)));
  float pow_w = exp2(C170.z * log2(abs(K_ONE - sat_w)));
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

  // ---- 5. Highlight, rim and reflections ----
  // Highlight along the half vector between the normal and the view vector, strongest at grazing angles.
  vec3 half_sum = (normal + view) * K_HALF;
  vec3 half_dir = half_sum * InvLength(dot(half_sum, half_sum));
  float spec_dot = max(dot(half_dir, C12.xyz), K_EPS);
  float edge = K_ONE - abs(n_dot_v);                                         // 0 facing the camera, 1 at the silhouette
  float edge_pow = exp2(C205.y * log2(abs(edge)));
  vec3 spec = (spec_dot * edge_pow) * C4.xyz * C205.x * in_light_pos.w;

  // The base plus the highlight, mixed by the two weights, and added back scaled by the rim strength: the rim is the
  // edge term times the arena's lighting map term, lifted by the normal map's w.
  vec3 spec_mix = (base + spec) * C253.y - base + (base * (spec + K_ONE)) * C253.w;
  float lightmap_term = max(lightmap.y - in_light_pos.z, K_EPS) * C162.z + lightmap.x;
  float lightmap_pow = exp2(C161.x * log2(abs(lightmap_term)));
  float spec_mask_mix = (normal1.w - K_ONE) * C161.y + K_ONE;
  float rim_strength = (spec_mask_mix * (edge * lightmap_pow)) * C161.z;
  vec3 body = spec_mix * rim_strength + base;

  // Reflection cube along the reflected view vector (taken through the 3x3 in c52-c54): two channels, weighted and
  // masked by the specular mask (the first also by interpolator 9); plus, where the bump weight is on, the paint
  // area's own reflection (third channel, along the second normal's reflection).
  vec3 reflect_dir = vec3(dot(reflected, C52.xyz), dot(reflected, C53.xyz), dot(reflected, C54.xyz));
  vec2 reflect_xy = texture(samplerCube(t_reflect, s_reflect), reflect_dir).xy;
  float reflection = (reflect_xy.x * C197.w) * (spec_mask * C142.x) * in_reflect_gain.x + (reflect_xy.y * C197.w) * (spec_mask * C142.z);
  vec3 paint_dir = vec3(dot(reflected2, C52.xyz), dot(reflected2, C53.xyz), dot(reflected2, C54.xyz));
  vec3 paint_dx = dFdxCoarse(paint_dir), paint_dy = dFdyCoarse(paint_dir);  // outside the branch, for the fetch's gradients
  float paint_reflection = 0.0;
  if (bump_weight > 0.0) {
    float paint_cube = textureGrad(samplerCube(t_reflect, s_reflect), paint_dir, paint_dx, paint_dy).z;
    paint_reflection = (C197.w * paint_cube) * bump_weight;
  }
  float reflection_total = reflection + paint_reflection;

  // The sky seen at a glancing angle in the upward reflection: a rim colour, masked by the normal map's w.
  float sky_rim = exp2(C204.w * log2(abs(K_ONE + n_dot_v)));
  float sky_up = max((-reflected.y) * sky_rim, K_EPS);
  vec3 env = reflection_total * C144.xyz + (sky_up * C204.xyz) * normal1.w;
  vec3 env_lit = env * lit;

  // ---- 6. Sum, soft add of the reflections, fog ----
  vec3 color = body * (K_ONE - min(env_lit, K_ONE)) + env_lit;
  color = (C195.xyz - color) * in_uv_fog.z + color;
  XeOutputColor(vec4(color, in_vertex.w));
}
