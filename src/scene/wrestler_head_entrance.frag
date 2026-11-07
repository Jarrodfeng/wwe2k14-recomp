#version 450
#extension GL_GOOGLE_include_directive : require
#ifdef POWLEAN
#extension GL_EXT_control_flow_attributes : require
#endif
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md W2, the wrestlers' materials ours first): the wrestlers' head / face skin material
// in ENTRANCES, in place of guest pixel shader 1D20CB5D36B02E5B, through the shader swap (src\scene_swap.h; SDK
// vulkan/native/shader_swap.h). The draw keeps its pass, textures, samplers and float constants; the colour goes out
// through the draw's own alpha test / exponent bias / gamma step (xenos_ps.glsl, kFlagOwnEpilogue).
//
// The same material family as the match variant (wrestler_head.frag, guest shader FEE45804EDD0EC05) and written the same
// way, from the shader's data flow (the game's microcode read locally, checked against the SDK translator's own SPIR-V
// of it): plain multiplies (no Xenos 0 * inf rule, except the few places named Mul0 / PowAbs below, where the console's
// answer is kept on purpose - the lights' log2 / exp2 maths here is the kind the Intel black-eyes bug was traced to,
// src\intel_exact_mul.h), no texture sign switches (every texture is read unsigned, no exponent adjust,
// no fetch-constant LOD bias; the shader's own LOD bias on the colour texture, c201.w, is kept), the ambient cube looked
// up by direction (the guest builds a face coordinate with its `cube` instruction and the translator turns it back into
// a direction, so the direction goes straight in), no rounding nudge on the coordinates, and implicit derivatives (the
// fetches inside the paint branch take gradients worked out before it).
//
// What differs from the match variant: there is no reflection cube map and no 3x3 matrix for it (c52-c54 are gone);
// instead the reflections are four directional lights (a direction in c8-c11, a weight in the w of c0-c3), each raised
// to two lobe widths along the reflected view vector, with the paint area using its own single width (c210.y) - the same
// change the body material's entrance variant makes. What the mask texture's y and z channels and the area texture hold
// in the game's art is unconfirmed (they are named by how the shader uses them).
//
// What it computes, in order:
//   1. the area mask texture (paint mask in x, two more area masks in y and z), the colour texture tinted by the paint
//      mask, and the specular mask re-aimed where the area masks are on;
//   2. the surface normal: a basis made from the vertex normal and the incoming tangent, two normal maps, and - where
//      the paint strength is above 0 - the paint's own normal map and the mask's own bump (four taps of the mask);
//   3. the shadow: the shadow map compared against the pixel's light-space depth, two filter pairs, faded with N.L,
//      plus a per-vertex shadow term;
//   4. the light: a sky / ground ambient by the up axis plus the ambient cube map along the normal, shaded and warmed by
//      the glow, then softened above 1 and brightened by the glow;
//   5. a highlight along the half vector, a rim term scaled by the arena's lighting map, the four lights along the
//      reflected view vector (and again along the paint bump's reflection where the paint strength is above 0);
//   6. the sum, a soft add of the lights, and the fog.
//
// Float constants (set 1 binding 2): the guest shader's registers in ascending order - 0, 1, 2, 3, 4, 8, 9, 10, 11, 12,
// 142, 144, 155, 156, 158, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 199, 201, 204, 205, 210,
// 218, 230, 234, 246, 253, 254, 255 (41; registered as kWrestlerHeadEntranceFloats). 253-255 are the compiler's own
// literal bank (the guest shader loads them itself); what each holds is inferred from how it is used and is NOT
// captured, so they are read from the constants like any other (K_* below say what they appear to be).

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[41]; } fc;
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
#define C155 fc.c[12]  // xyz: warm colour the glow mixes towards; w: its strength
#define C156 fc.c[13]  // x: gain of the specular mask into the glow weight; y: light level the glow starts at
#define C158 fc.c[14]  // xyz: floor colour of the shadowed side
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
#define C199 fc.c[29]  // x: gain of the ambient cube's alpha into the glow
#define C201 fc.c[30]  // w: LOD bias of the colour texture
#define C204 fc.c[31]  // xyz: rim colour; w: rim power
#define C205 fc.c[32]  // x: highlight gain; y: fresnel power; z, w: the two specular-mask targets (mixed by the area mask z)
#define C210 fc.c[33]  // x: weight of the area mask in the paint strength; y: lobe width (power) of the paint area's own light; z: tiling of the paint normal map
#define C218 fc.c[34]  // xyz: tangent / bitangent / normal weights of the perturbed normal
#define C230 fc.c[35]  // xy, zw: corners of the second normal map's rectangle in texture-coordinate space
#define C234 fc.c[36]  // x: paint mask strength
#define C246 fc.c[37]  // xyz: paint colour
#define C253 fc.c[38]  // x: small floor / epsilon (expected about 0); y, z: mix weights of the highlight; w: first normal map's scale
#define C254 fc.c[39]  // literal bank: x expected 1, y expected 1.5 (builds the guest's cube coordinate: not read here), z expected -1, w expected 0.5
#define C255 fc.c[40]  // x, w: tap offsets in u; y, z: tap offsets in v (the mask's four taps)

#define K_EPS       C253.x  // floor of the clamps (about 0)
#define K_ONE       C254.x
#define K_MINUS_ONE C254.z
#define K_HALF      C254.w

// Textures and samplers under the canonical set-3 bindings (1000 + fetch constant, 1300 + for cubes, 1200 + samplers).
layout(set = 3, binding = 1000) uniform texture2DArray t_albedo;      // fetch constant 0: colour
layout(set = 3, binding = 1001) uniform texture2DArray t_normal1;     // 1: first normal map (x, y) and specular mask (w)
layout(set = 3, binding = 1004) uniform texture2DArray t_paint_normal;// 4: the paint's own normal map (x, y), tiled by c210.z
layout(set = 3, binding = 1005) uniform texture2DArray t_normal2;     // 5: second normal map (x in w, y in y), in the rectangle c230
layout(set = 3, binding = 1006) uniform texture2DArray t_area;        // 6: an area mask (w)
layout(set = 3, binding = 1012) uniform texture2DArray t_shadow;      // 12: shadow map (four depths)
layout(set = 3, binding = 1014) uniform texture2DArray t_mask;        // 14: paint mask (x) and two area masks (y, z)
layout(set = 3, binding = 1015) uniform texture2DArray t_lightmap;    // 15: the arena's lighting map
layout(set = 3, binding = 1313) uniform textureCube t_ambient;        // 13: ambient (diffuse) cube, alpha = glow
layout(set = 3, binding = 1200) uniform sampler s_albedo;
layout(set = 3, binding = 1201) uniform sampler s_normal1;
layout(set = 3, binding = 1204) uniform sampler s_paint_normal;
layout(set = 3, binding = 1205) uniform sampler s_normal2;
layout(set = 3, binding = 1206) uniform sampler s_area;
layout(set = 3, binding = 1212) uniform sampler s_shadow;
layout(set = 3, binding = 1214) uniform sampler s_mask;
layout(set = 3, binding = 1215) uniform sampler s_lightmap;
layout(set = 3, binding = 1213) uniform sampler s_ambient;

// The guest's interpolators 0-7 and 9 (8 is not read).
layout(location = 0) in vec4 in_normal;     // xyz: vertex normal
layout(location = 1) in vec4 in_uv_fog;     // xy: texture coordinate; z: fog amount; w: weight of the second normal map
layout(location = 2) in vec4 in_view;       // xyz: view vector (eye to surface)
layout(location = 3) in vec4 in_shadow_pos; // xy / w: shadow map coordinate; z: light-space depth; w: projection divisor
layout(location = 4) in vec4 in_shadow_add; // x: a per-vertex term added to the shadow before it is clamped
layout(location = 5) in vec4 in_light_pos;  // xy: lighting map coordinate; z: height; w: highlight scale
layout(location = 6) in vec4 in_tangent;    // xyz: tangent
layout(location = 7) in vec4 in_vertex;     // xyz: vertex light colour; w: alpha
layout(location = 9) in vec4 in_gloss;      // x: weight of the lights at the first lobe width

#define SAMPLE2D(tex, smp, uv) texture(sampler2DArray(tex, smp), vec3((uv), 0.0))
#define SAMPLE2D_BIAS(tex, smp, uv, bias) texture(sampler2DArray(tex, smp), vec3((uv), 0.0), (bias))

// The console's multiply: zero times anything (infinity and NaN included) is zero. Plain maths gives 0 * infinity = NaN,
// the likely cause (unconfirmed) of the black eyes on an Intel GPU that flushes tiny values to zero
// (src\intel_exact_mul.h), so the rule is kept at every product here that can meet an infinity: the guest's reciprocals
// and its log2 maths.
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
#ifdef POWLEAN
  // (Session 95, r3gpu: Mul0 only differs from the plain multiply at width 0, a uniform: log2(0) x 0. Every other width takes the plain multiply.)
  vec4 lobe;
  [[dont_flatten]] if (width != 0.0) {
    lobe = exp2(light_logs * width);
  } else {
    lobe = vec4(exp2(Mul0(light_logs.x, width)), exp2(Mul0(light_logs.y, width)), exp2(Mul0(light_logs.z, width)), exp2(Mul0(light_logs.w, width)));
  }
#else
  vec4 lobe = vec4(exp2(Mul0(light_logs.x, width)), exp2(Mul0(light_logs.y, width)),
                   exp2(Mul0(light_logs.z, width)), exp2(Mul0(light_logs.w, width)));
#endif
  float sum = lobe.w * C3.w;
  sum = lobe.z * C2.w + sum;
  sum = lobe.y * C1.w + sum;
  return lobe.x * C0.w + sum;
}

// Session 95 (r3gpu): the powers with a whole exponent (wrestler_pow.glsl, gpu_native_wrestler_pow). Plain compile: the material's own pow, as before.
#ifdef POWLEAN
#define POW_ANY(x, p) PowAbs(x, p)
#include "wrestler_pow.glsl"
#define LPOW1(x, p) PowLean1(x, p)
#define LPOW2(x, p) PowLean2(x, p)
#define LPOW4(x, p) PowLean4(x, p)
#define LPOW20(x, p) PowLean20(x, p)
#else
#define LPOW1(x, p) PowAbs(x, p)
#define LPOW2(x, p) PowAbs(x, p)
#define LPOW4(x, p) PowAbs(x, p)
#define LPOW20(x, p) PowAbs(x, p)
#endif

void main() {
  vec2 uv = in_uv_fog.xy;
  vec3 vertex_normal = in_normal.xyz;
  vec3 view = in_view.xyz;
  vec3 tangent = in_tangent.xyz;
  vec3 bitangent = cross(vertex_normal, tangent);

  // ---- 1. Masks, colour and the specular mask ----
  vec3 mask = SAMPLE2D(t_mask, s_mask, uv).xyz;          // x: paint mask; y, z: area masks
  float paint_mask = mask.x;
  // The second normal map lives in a rectangle of texture-coordinate space (c230): the coordinate rescaled to it.
  // (The guest divides by the rectangle's size with a reciprocal; a zero size gives an infinity, and 0 * infinity stays 0.)
  vec2 rect_size = C230.zw - C230.xy;
  vec2 rect_uv = vec2(Mul0(uv.x - C230.x, 1.0 / rect_size.x), Mul0(uv.y - C230.y, 1.0 / rect_size.y));
  vec3 albedo_tex = SAMPLE2D_BIAS(t_albedo, s_albedo, uv, C201.w).rgb;
  float area = SAMPLE2D(t_area, s_area, uv).w;
  vec2 normal2_tex = SAMPLE2D(t_normal2, s_normal2, rect_uv).wy;   // DXT5nm style: x in alpha, y in green
  vec4 normal1 = SAMPLE2D(t_normal1, s_normal1, uv);     // x, y: normal map; w: specular mask
  vec2 lightmap = SAMPLE2D(t_lightmap, s_lightmap, in_light_pos.xy).xy;

  // Where the second area mask (y) and the area texture are both on, the specular mask is re-aimed at a target that
  // fades out under the paint (the target itself is mixed from two constants by the third area mask).
  float area_band = mask.y * area;
  float spec_target = ((C205.z - C205.w) * mask.z + C205.w) * (K_ONE - paint_mask);
  float spec_mask = area_band * (spec_target - normal1.w) + normal1.w;

  // The paint strength: the paint mask (squared-ish: strength times mask, then mixed in by the mask itself) plus the area
  // band's own share. Above 0 the paint branch below runs.
  float area_share = C210.x * area_band;
  float paint_strength = (paint_mask * C234.x - area_share) * paint_mask + area_share;
  bool paint_on = paint_strength > 0.0;

  // The colour: tinted by the paint colour where the mask is on, times the light colour.
  vec3 tint = (C246.xyz + K_MINUS_ONE) * paint_mask + K_ONE;
  vec3 diffuse = in_vertex.xyz * (tint * (albedo_tex * C190.xyz));

  // ---- 2. The normal ----
  // Offsets from the first normal map and from the second (its value unpacked to a signed number the other way round),
  // weighted by the vertex blend value.
  float base_u = normal1.y * C253.w + K_ONE;
  float base_v = normal1.x * C253.w + K_ONE;
  float second_u = (normal2_tex.x + normal2_tex.x) * K_MINUS_ONE + K_ONE;
  float second_v = (normal2_tex.y + normal2_tex.y) * K_MINUS_ONE + K_ONE;
  float blend = in_uv_fog.w;
  float u = second_u * blend + base_u;
  float v = second_v * blend + base_v;
  vec3 normal = PerturbedNormal(tangent, bitangent, vertex_normal, u, v);

  // The normal the paint area's light uses: where the paint strength is above 0, the paint's own normal map and the
  // mask's own bump (four taps of the mask texture around the coordinate) go into the offsets; elsewhere it is the same
  // normal.
  vec2 uv_dx = dFdxCoarse(uv), uv_dy = dFdyCoarse(uv);  // outside the branch: the fetches inside use these as gradients
  vec2 paint_uv = uv * C210.z;
  vec2 paint_dx = dFdxCoarse(paint_uv), paint_dy = dFdyCoarse(paint_uv);
  vec3 normal2 = normal;
  if (paint_on) {
    float tap0 = textureGrad(sampler2DArray(t_mask, s_mask), vec3(uv.x + C255.x, uv.y + C255.y, 0.0), uv_dx, uv_dy).x;
    float tap1 = textureGrad(sampler2DArray(t_mask, s_mask), vec3(uv.x + C255.x, uv.y + C255.z, 0.0), uv_dx, uv_dy).x;
    float tap2 = textureGrad(sampler2DArray(t_mask, s_mask), vec3(uv.x + C255.w, uv.y + C255.z, 0.0), uv_dx, uv_dy).x;
    float tap3 = textureGrad(sampler2DArray(t_mask, s_mask), vec3(uv.x + C255.w, uv.y + C255.y, 0.0), uv_dx, uv_dy).x;
    vec2 paint_tex = textureGrad(sampler2DArray(t_paint_normal, s_paint_normal), vec3(paint_uv, 0.0), paint_dx, paint_dy).xy;
    float d0 = paint_mask - tap0, d1 = paint_mask - tap1, d2 = paint_mask - tap2, d3 = paint_mask - tap3;
    float grad_u = (d2 * K_MINUS_ONE + d1 * K_MINUS_ONE + d0) + d3;                          // along the bitangent side
    float grad_v = d3 * K_MINUS_ONE + d2 * K_MINUS_ONE + d1 * K_ONE + d0 * K_ONE;             // along the tangent side
    float paint_u = (paint_tex.y + paint_tex.y) * K_MINUS_ONE + K_ONE;                       // the paint map, signed (y flipped)
    float paint_v = (paint_tex.x + paint_tex.x) * K_ONE + K_MINUS_ONE;                       // (x as is)
    float u2 = (area_band * paint_u + u) + grad_u * -paint_mask;
    float v2 = (area_band * paint_v + v) + grad_v * -paint_mask;
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
  float shade_x = K_ONE - LPOW4(K_ONE - sat_x, C169.w);
  float shade_z = K_ONE - LPOW4(K_ONE - sat_z, C169.w);
  // Pair 2 (y, w channels) the same with an offset and its own power; each mixes into its pair-1 partner.
  float sat_y = clamp((depth_diff.y - C170.x) * C170.y, 0.0, 1.0);
  float sat_w = clamp((depth_diff.w - C170.x) * C170.y, 0.0, 1.0);
  float pow_y = LPOW20(K_ONE - sat_y, C170.z);
  float pow_w = LPOW20(K_ONE - sat_w, C170.z);
  float shadow_a = (shade_x * pow_y - shade_x) * C170.w + shade_x;
  float shadow_b = (shade_z * pow_w - shade_z) * C170.w + shade_z;
  float shadow_amount = max(shadow_a, shadow_b);
  float shadow_fade = K_ONE - (K_ONE - n_dot_l) * C169.y;                 // surfaces facing away from the light shadow less
  float lit = K_ONE - clamp((C168.w * shadow_amount) * shadow_fade + in_shadow_add.x, 0.0, 1.0);  // 1 = fully lit

  // ---- 4. Light: sky / ground ambient plus the ambient cube with the skin glow ----
  vec4 ambient_tex = texture(samplerCube(t_ambient, s_ambient), normal);
  vec3 ambient_cube = min(ambient_tex.rgb * C197.xyz, C187.xyz);
  float up = dot(-normal, C180.xyz);
  float up_mix = K_HALF * up + K_HALF;                                      // 0 facing up, 1 facing down
  vec3 sky_ground = (C182.xyz - C181.xyz) * up_mix + C181.xyz;
  // The shadowed side keeps a floor colour (c158); the cube's alpha makes a glow amount, which pulls the light towards
  // the warm colour (c155) by that colour's strength.
  vec3 lit_floor = (K_ONE - C158.xyz) * lit + C158.xyz;
  vec3 shaded_ambient = (ambient_cube * lit) * lit_floor;
  float glow_amount = (C199.x * ambient_tex.w) * lit;
  vec3 warm = ((C155.xyz - C158.xyz) * glow_amount + C158.xyz) * C155.w;
  vec3 ambient_light = shaded_ambient * (K_ONE - warm) + warm * glow_amount;
  vec3 light = sky_ground * C189.xyz + ambient_light;
  // Where the specular mask is high, light above 1 is softened and a glow lightens the colour, starting at c156.y.
  float glow_weight = min(C156.x * normal1.w, K_ONE);
  vec3 light_capped = min(light, vec3(K_ONE));
  vec3 light_soft = (light - light_capped) * (K_ONE - glow_weight) + light_capped;
  vec3 glow = clamp((light - C156.y) * glow_weight, 0.0, 1.0);
  vec3 lit_colour = diffuse * light_soft;
  vec3 base = glow * (K_ONE - lit_colour) + lit_colour;                     // the lit skin colour

  // ---- 5. Highlight, rim and the four lights ----
  // Highlight along the half vector between the normal and the view vector, strongest at grazing angles.
  vec3 half_sum = (normal + view) * K_HALF;
  vec3 half_dir = half_sum * InvLength(dot(half_sum, half_sum));
  float spec_dot = max(dot(half_dir, C12.xyz), K_EPS);
  float edge = K_ONE - abs(n_dot_v);                                         // 0 facing the camera, 1 at the silhouette
  float edge_pow = LPOW2(edge, C205.y);
  vec3 spec = (spec_dot * edge_pow) * C4.xyz * C205.x * in_light_pos.w;

  // The base plus the highlight, mixed by the two weights, and added back scaled by the rim strength: the rim is the
  // edge term times the arena's lighting map term, lifted by the specular mask.
  vec3 spec_mix = (base + spec) * C253.y - base + (base * (spec + K_ONE)) * C253.z;
  float lightmap_term = max(lightmap.y - in_light_pos.z, K_EPS) * C162.z + lightmap.x;
  float lightmap_pow = LPOW1(lightmap_term, C161.x);
  float spec_mask_mix = (normal1.w - K_ONE) * C161.y + K_ONE;
  float rim_strength = (spec_mask_mix * (edge * lightmap_pow)) * C161.z;
  vec3 body = spec_mix * rim_strength + base;

  // The four lights along the reflected view vector: each clamped dot, its log, then two lobe widths (c142.y and
  // c142.w), weighted by the re-aimed specular mask and the two lobe weights (c142.x and c142.z), the first also by a
  // vertex value.
  vec4 light_dots = vec4(dot(reflected, C8.xyz), dot(reflected, C9.xyz), dot(reflected, C10.xyz), dot(reflected, C11.xyz));
  vec4 light_logs = log2(abs(max(light_dots, K_EPS)));
  float lobes_a = WeightedLobes(light_logs, C142.y);
  float lobes_b = WeightedLobes(light_logs, C142.w);
  float spec_light = ((spec_mask * C142.x) * lobes_a) * in_gloss.x + (spec_mask * C142.z) * lobes_b;

  // Where the paint strength is above 0, the paint area's own light: the same four lights along the paint bump's
  // reflection, one lobe width (c210.y), scaled by the paint strength. Zero elsewhere.
  float paint_light = 0.0;
  if (paint_on) {
    vec4 paint_dots = vec4(dot(reflected2, C8.xyz), dot(reflected2, C9.xyz), dot(reflected2, C10.xyz), dot(reflected2, C11.xyz));
    vec4 paint_logs = log2(abs(max(paint_dots, K_EPS)));
    paint_light = paint_strength * WeightedLobes(paint_logs, C210.y);
  }
  float light_total = spec_light + paint_light;

  // The sky seen at a glancing angle in the upward reflection: a rim colour, masked by the specular mask.
  float sky_rim = LPOW1(K_ONE + n_dot_v, C204.w);
  float sky_up = max((-reflected.y) * sky_rim, K_EPS);
  vec3 env = light_total * C144.xyz + (sky_up * C204.xyz) * normal1.w;
  vec3 env_lit = env * lit;

  // ---- 6. Sum, soft add of the lights, fog ----
  vec3 color = body * (K_ONE - min(env_lit, K_ONE)) + env_lit;
  color = (C195.xyz - color) * in_uv_fog.z + color;
  XeOutputColor(vec4(color, in_vertex.w));
}
