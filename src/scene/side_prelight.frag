#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the entrance's lighting cubes (the game's
// yPreLight, guest pixel shader 8479D8DA0BB01D6B; five 400x64 passes a frame, 6 faces of 64x64 each), through the shader
// swap (src\scene_swap.h): for the direction of each cube texel, a colour from four lights. No textures.
// In order: the texel direction (the interpolator, xyz) is normalised by the guest's reciprocal root (kept with its
// 0 x infinity = 0). Each of four lights has a direction (c8..c11), whose cosine to the texel direction is taken, and a
// colour (c0..c3), whose luminance is its dot product with c255.xyz (luma weights, unconfirmed). Each cosine goes through
// two lobes (1: c204/c205/c206, 2: c207/c208/c209): the distance from the lobe's threshold (c204.w / c207.w) is folded
// back onto it, taken from the lobe's edge, scaled by its width factor, offset, rooted and scaled again into a curve
// parameter, which runs through a quadratic Bezier of three control values (c205.xyz / c208.xyz) with c254.x added (the
// first light's value is built with the compiler's literal offsets c254.x / c254.y, kept as written). Each lobe's value
// is signed by the side of its threshold the cosine is on. The two lobes' values are blended per light (against c255.w
// and c254.w); the blend is pulled towards the lobes by (1 - the strongest light's share of the total luminance times
// (c254.w - c16..c19.x)); the colours are weighted and summed times c198.xyz; alpha is c199.y times the luminance of
// lobe 1's blend after that pull (what the cube stores it for: unconfirmed). Arithmetic is kept as the translation's
// (separate multiply and add, a fused multiply-add only where it has one), so the sums come out the same bit for bit.
// Float constants (set 1 binding 2): 0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 198, 199, 204, 205, 206, 207, 208,
// 209, 254, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[22]; } fc;
#define C0   fc.c[0]   // xyz: light 0's colour
#define C1   fc.c[1]   // light 1's colour
#define C2   fc.c[2]   // light 2's colour
#define C3   fc.c[3]   // light 3's colour
#define C8   fc.c[4]   // xyz: light 0's direction
#define C9   fc.c[5]   // light 1's direction
#define C10  fc.c[6]   // light 2's direction
#define C11  fc.c[7]   // light 3's direction
#define C16  fc.c[8]   // x: light 0's share kept (1 - x is its falloff, unconfirmed)
#define C17  fc.c[9]   // x: light 1's
#define C18  fc.c[10]  // x: light 2's
#define C19  fc.c[11]  // x: light 3's
#define C198 fc.c[12]  // xyz: output colour scale
#define C199 fc.c[13]  // y: alpha scale
#define C204 fc.c[14]  // lobe 1: x: width factor, y: offset, z: edge, w: threshold on the cosine
#define C205 fc.c[15]  // lobe 1: xyz: the Bezier's three control values
#define C206 fc.c[16]  // lobe 1: y: root input offset, z: scale
#define C207 fc.c[17]  // lobe 2: x: width factor, y: offset, z: edge, w: threshold
#define C208 fc.c[18]  // lobe 2: xyz: the Bezier's control values
#define C209 fc.c[19]  // lobe 2: y: root input offset, z: scale
#define C254 fc.c[20]  // x, y: the compiler's literal offsets (0.5 and 0 unconfirmed), z: lobe factor, w: one (unconfirmed)
#define C255 fc.c[21]  // xyz: luma weights (unconfirmed), w: blend bias (the compiler's literal bank)

layout(location = 0) in vec4 in_dir;  // xyz: the cube texel's direction

// The console's multiply: a NaN product (0 x infinity) is 0.
float Mul0(float a, float b) {
  precise float p = a * b;
  return p != p ? 0.0 : p;
}

// A light's axis (already holding the reciprocal root) against the texel direction; terms added z, x, y as the guest does.
float AxisDot(vec3 axis, float inv_len, vec3 d) {
  vec3 s = vec3(Mul0(inv_len, axis.x), Mul0(inv_len, axis.y), Mul0(inv_len, axis.z));
  return (Mul0(s.z, d.z) + Mul0(s.x, d.x)) + Mul0(s.y, d.y);
}

// A colour's luminance: its channels against the luma weights, accumulated z-weight first as the guest does.
float Luma(vec3 colour, vec3 w) {
  precise float p = colour.z * w.x;
  return fma(colour.y, w.z, fma(colour.x, w.y, p));
}

// One lobe for one light: cosine -> the curve parameter (before the Bezier) and the side of the threshold.
float LobeParam(float cos_angle, float threshold, float edge, float width, float root_offset, float offset, float scale,
                out float side) {
  precise float s = cos_angle - threshold;
  precise float folded = clamp(abs(s) + threshold, 0.0, 1.0);
  float m = fma(-width, edge - folded, root_offset);
  side = float(s > -s) - float(-s > s);
  precise float t = (-sqrt(abs(m)) - offset) * scale;
  return t;
}

// The quadratic Bezier of three control values at t, then the guest's offset (c254.x) added after.
float Bezier(float t, vec3 p) {
  precise float q0 = t * (p.y - p.x) + p.x;
  precise float q1 = t * (p.z - p.y) + p.y;
  precise float r = t * (q1 - q0) + q0;
  return r;
}

// The same for the first light, with the compiler's literal offsets c254.x / c254.y folded in as written (the offsets
// that appear on both sides cancel to c254.x).
float BezierFirst(float t, vec3 p) {
  float d0 = p.y - p.x, d1 = p.z - p.y;
  precise float a = (t * d0 + C254.x) + p.x;
  precise float b = (t * d0 + C254.y) + p.x;
  precise float c = (t * d1 + C254.y) + p.y;
  precise float r = t * (c - b) + a;
  return r;
}

void main() {
  vec3 d = in_dir.xyz;
  precise float len_z = d.z * d.z;
  float len2 = fma(d.y, d.y, fma(d.x, d.x, len_z));
  float inv_len = inversesqrt(abs(len2));

  vec4 cosv = vec4(AxisDot(C8.xyz, inv_len, d), AxisDot(C9.xyz, inv_len, d), AxisDot(C10.xyz, inv_len, d),
                   AxisDot(C11.xyz, inv_len, d));
  vec4 lum = vec4(Luma(C0.xyz, C255.xyz), Luma(C1.xyz, C255.xyz), Luma(C2.xyz, C255.xyz), Luma(C3.xyz, C255.xyz));
  precise vec4 keep_base = C254.w - vec4(C16.x, C17.x, C18.x, C19.x);

  float lobe1_width = C254.z * C204.x;
  float lobe2_width = C207.x * C254.z;
  vec4 t1, side1, t2, side2;
  for (int i = 0; i < 4; ++i) {
    t1[i] = LobeParam(cosv[i], C204.w, C204.z, lobe1_width, C206.y, C204.y, C206.z, side1[i]);
    t2[i] = LobeParam(cosv[i], C207.w, C207.z, lobe2_width, C209.y, C207.y, C209.z, side2[i]);
  }
  precise vec4 w1 = vec4(BezierFirst(t1.x, C205.xyz), Bezier(t1.y, C205.xyz) + C254.x, Bezier(t1.z, C205.xyz) + C254.x,
                 Bezier(t1.w, C205.xyz) + C254.x);
  precise vec4 w2 = vec4(BezierFirst(t2.x, C208.xyz), Bezier(t2.y, C208.xyz) + C254.x, Bezier(t2.z, C208.xyz) + C254.x,
                 Bezier(t2.w, C208.xyz) + C254.x);

  precise vec4 k2 = w2 * side2 + C255.w;                // lobe 2 on the bias
  precise vec4 k2_one = w2 * side2 + C254.w;            // lobe 2 on one
  precise vec4 p = w1 * side1 + C255.w;                 // lobe 1 on the bias
  precise vec4 q = w1 * side1 + k2_one;                 // both lobes on one
  precise vec4 e2 = q * C255.w - k2;
  precise vec4 e1 = q * C255.w - p;

  // Luminance of lobe 1's blend, summed w, z, x, y; the strongest light's share of it.
  precise float total = ((p.w * lum.w + p.z * lum.z) + p.x * lum.x) + p.y * lum.y;
  precise vec4 lit = p * lum;
  float strongest = max(max(lit.x, lit.y), max(lit.z, lit.w));
  float share = Mul0(strongest, 1.0 / total);
  precise vec4 keep = -share * keep_base + C254.w;

  precise vec4 shaded = e1 * keep + p;   // lobe 1's blend after the falloff (its luminance goes to alpha)
  precise vec4 weight = e2 * keep + k2;  // each light's weight

  precise vec3 colour = weight.w * C3.xyz;
  colour = weight.z * C2.xyz + colour;
  colour = weight.y * C1.xyz + colour;
  colour = weight.x * C0.xyz + colour;
  precise float shaded_lum = ((shaded.w * lum.w + shaded.z * lum.z) + shaded.x * lum.x) + shaded.y * lum.y;
  XeOutputColor(vec4(colour * C198.xyz, Mul0(C199.y, shaded_lum)));
}
