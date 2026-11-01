#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the blur of the wrestlers' plane-shadow mask
// (the game's yBlur, guest pixel shader E1AD4BB4E0563253; a 256x256 single-channel target, drawn twice a frame in
// matches), through the shader swap (src\scene_swap.h). 21 taps of one texture: the pixel's own coordinate and two rings
// of ten points around it. The three interpolators are clip-space positions (x, y, w; z unused): the pixel's, and two
// neighbours that give the two directions of the rings. The ten ring points are the pixel plus k times each direction
// for the four k of c255 (eight points) and the two neighbours themselves; every point is divided by its w and mapped
// to texture coordinates (0.5 * x / w + 0.5, 0.5 - 0.5 * y / w), the divisions kept as the guest's reciprocal times
// the numerator with its 0 x infinity = 0.
// Ring 1: each point's offset from the pixel (the pixel's coordinate plus c254.w, minus the point's) scaled by c253.z
// and moved by c253.x; the red of the ten taps and of the pixel's own tap is summed and turned into a shrink factor
// t = c254.y - saturate(sum * c253.y - c34.x). Ring 2: the same ten offsets (unscaled) times t, taken from the pixel's
// coordinate plus c254.z; all four channels of the ten taps and the pixel's own are summed and times c253.y.
// The tap sums are added in the guest's order. Every tap is read at the LOD bias the first fetch constant gives (word 1,
// signed bits 12-21, times 1/32) and scaled by the power of two it gives (signed bits 13-18), as the translation does
// for all 21 taps: kept (the exact power of two is applied to each sum). The translator's coordinate nudge of
// 0.00146484375 / size is dropped. Float constants (set 1 binding 2): 34, 253, 254, 255. Fetch constants (set 1
// binding 4): word 1 of fetch constant 0 only.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[4]; } fc;
#define C34  fc.c[0]  // x: the coverage sum's threshold (unconfirmed)
#define C253 fc.c[1]  // x: offset of ring 1's offsets, y: tap weight / coverage scale (1/21 unconfirmed), z: ring 1's scale
#define C254 fc.c[2]  // x: 0.5 (clip to texture), y: v scale (unconfirmed), z: ring 2's pixel offset, w: ring 1's
#define C255 fc.c[3]  // xyzw: the four distances along each direction (the compiler's literal bank)

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;

layout(set = 3, binding = 1000) uniform texture2DArray t_source;  // fetch constant 0 (the mask)
layout(set = 3, binding = 1200) uniform sampler s_source;

layout(location = 0) in vec4 in_p0;  // xyw: the pixel's clip position
layout(location = 1) in vec4 in_p1;  // xyw: the first direction's neighbour
layout(location = 2) in vec4 in_p2;  // xyw: the second direction's neighbour

// The console's multiply: a NaN product (0 x infinity) is 0.
float Mul0(float a, float b) {
  precise float p = a * b;
  return p != p ? 0.0 : p;
}

// A clip position (x, y, w) as a texture-space offset from the centre: 0.5 * x / w and -0.5 * y / w (before + 0.5).
vec2 ClipToScreen(vec3 p) {
  float inv = 1.0 / p.z;
  return vec2(Mul0(Mul0(inv, C254.x), p.x), Mul0(-Mul0(p.y, Mul0(inv, C254.y)), C254.x));
}

// The ring point at distance k along a direction d from the pixel p, as a texture coordinate.
vec2 RingPoint(vec3 p, vec3 d, float k) {
  vec3 q = fma(d, vec3(k), p);
  float inv = 1.0 / q.z;
  float u = Mul0(Mul0(inv, C254.x), q.x) + C254.x;
  float v = Mul0(-Mul0(q.y, inv), C254.x) + C254.x;
  return vec2(u, v);
}

// Ring 1's tap: the pixel's coordinate minus the offset (scaled and moved).
vec2 Ring1(vec2 centre, vec2 point_uv) {
  vec2 d = centre - point_uv;
  return centre - vec2(Mul0(d.x, C253.z) + C253.x, Mul0(d.y, C253.z) + C253.x);
}

// Ring 2's tap: the shifted pixel coordinate minus t times the offset (a fused multiply-add for two of the points, as
// the guest has them).
vec2 Ring2Fma(vec2 base, vec2 d, float t) { return vec2(fma(-t, d.x, base.x), fma(-t, d.y, base.y)); }
vec2 Ring2Mul(vec2 base, vec2 d, float t) { return base - vec2(Mul0(t, d.x), Mul0(t, d.y)); }

// Fetch constant 0's LOD bias (read by every tap).
float g_lod_bias;
vec4 Tap(vec2 uv) { return texture(sampler2DArray(t_source, s_source), vec3(uv, 0.0), g_lod_bias); }

void main() {
  int fetch_word = int(xe_fetch.fetch_constants[1].x);
  g_lod_bias = float(bitfieldExtract(fetch_word, 12, 10)) * 0.03125;
  float exp_scale = ldexp(1.0, bitfieldExtract(fetch_word, 13, 6));
  vec3 p0 = vec3(in_p0.x, in_p0.y, in_p0.w);
  vec3 p1 = vec3(in_p1.x, in_p1.y, in_p1.w);
  vec3 p2 = vec3(in_p2.x, in_p2.y, in_p2.w);
  vec3 d1 = p1 - p0;
  vec3 d2 = p2 - p0;

  vec2 pixel = ClipToScreen(p0);
  vec2 centre1 = pixel + C254.w;  // ring 1's (and the pixel tap's) coordinate
  vec2 centre2 = pixel + C254.z;  // ring 2's

  // The ten ring points, in the order the guest sums them: for k = y, z, w, x the first direction's point then the
  // second's, then the two neighbours themselves.
  vec2 h[10];
  h[0] = RingPoint(p0, d1, C255.y);
  h[1] = RingPoint(p0, d2, C255.y);
  h[2] = RingPoint(p0, d1, C255.z);
  h[3] = RingPoint(p0, d2, C255.z);
  h[4] = RingPoint(p0, d1, C255.w);
  h[5] = RingPoint(p0, d2, C255.w);
  h[6] = RingPoint(p0, d1, C255.x);
  h[7] = RingPoint(p0, d2, C255.x);
  h[8] = ClipToScreen(p1) + C254.x;
  h[9] = ClipToScreen(p2) + C254.x;

  // Offsets of every point from ring 1's centre.
  vec2 off[10];
  for (int i = 0; i < 10; ++i) off[i] = centre1 - h[i];

  vec4 own = Tap(centre1);
  float sum = Tap(Ring1(centre1, h[0])).x;
  for (int i = 1; i < 10; ++i) sum += Tap(Ring1(centre1, h[i])).x;
  sum = (sum + own.x) * exp_scale;
  float t = C254.y - clamp(fma(sum, C253.y, -C34.x), 0.0, 1.0);

  vec4 total = Tap(Ring2Mul(centre2, off[0], t));
  total += Tap(Ring2Mul(centre2, off[1], t));
  total += Tap(Ring2Mul(centre2, off[2], t));
  total += Tap(Ring2Mul(centre2, off[3], t));
  total += Tap(Ring2Mul(centre2, off[4], t));
  total += Tap(Ring2Mul(centre2, off[5], t));
  total += Tap(Ring2Fma(centre2, off[6], t));
  total += Tap(Ring2Fma(centre2, off[7], t));
  total += Tap(Ring2Mul(centre2, off[8], t));
  total += Tap(Ring2Mul(centre2, off[9], t));
  total += own;
  XeOutputColor(total * exp_scale * C253.y);
}
