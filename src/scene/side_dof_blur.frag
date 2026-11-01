#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the match's depth-of-field blur (the game's
// yBlur, guest pixel shader A5C9AC54E4868DC1; docs\systems\post_effects.md, "Match" pass 2), through the shader swap
// (src\scene_swap.h): a thirteen-tap weighted blur of the downsample's output (side_dof_downsample.frag), run only on
// the pixels whose own alpha is not above zero (the out-of-focus ones; the downsample sets alpha 1 inside the in-focus
// band, unconfirmed as a name); every other pixel passes through as read (colour and alpha). The guest runs this with
// the console's predicate stack (a counter, r0.w, and a predicate), kept here as the same small machine: the pixel's
// own tap and its alpha open the blur (the counter is c255.y + 1, or 0 when the blur is on); then for each of the
// twelve offsets c1..c12, in order, that tap is read only while the machine is still open, a flag is set to 1 when
// c255.x is above the tap's alpha (the tap is itself out of focus), and only a flagged tap is added: the pixel's own
// colour times c16.x, then each flagged tap's colour times its weight c17..c28 (xyz, one fused multiply-add each), the
// weights' x summed alongside (the first sum is c16.x + c17.x as the guest writes it). The blur's colour is the sum
// times 1 / that weight total (the console's 0 x infinity = 0 kept), alpha 1. The fetch constant's LOD bias and
// exponent adjust are kept as the guest applies them (the draw's own fetch constants, set 1 binding 4).
// Float constants (set 1 binding 2): 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
// 26, 27, 28, 255. (c0 is only named by a no-op in the guest and is not read.)

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[27]; } fc;
#define C1   fc.c[1]   // xy: offsets of the taps (c1..c12)
#define C2   fc.c[2]
#define C3   fc.c[3]
#define C4   fc.c[4]
#define C5   fc.c[5]
#define C6   fc.c[6]
#define C7   fc.c[7]
#define C8   fc.c[8]
#define C9   fc.c[9]
#define C10  fc.c[10]
#define C11  fc.c[11]
#define C12  fc.c[12]
#define W16  fc.c[13]  // xyz: weights of the pixel's own tap (x) and of the taps (c17..c28)
#define W17  fc.c[14]
#define W18  fc.c[15]
#define W19  fc.c[16]
#define W20  fc.c[17]
#define W21  fc.c[18]
#define W22  fc.c[19]
#define W23  fc.c[20]
#define W24  fc.c[21]
#define W25  fc.c[22]
#define W26  fc.c[23]
#define W27  fc.c[24]
#define W28  fc.c[25]
#define C255 fc.c[26]  // x: an out-of-focus tap's alpha limit; y: zero (the machine's start; the compiler's literal bank)

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;
#define FETCH0_WORD int(xe_fetch.fetch_constants[1].x)
float LodBias(int word) { return float(bitfieldExtract(word, 12, 10)) * 0.03125; }
float ExponentScale(int word) { return ldexp(1.0, bitfieldExtract(word, 13, 6)); }

layout(set = 3, binding = 1000) uniform texture2DArray t_source;  // fetch constant 0: the downsample's output
layout(set = 3, binding = 1200) uniform sampler s_source;

layout(location = 0) in vec4 in_uv;  // xy: the pixel's coordinate in the source; z: only read if the machine never opens

vec4 Tap(vec2 at) {
  int word = FETCH0_WORD;
  return texture(sampler2DArray(t_source, s_source), vec3(at, 0.0), LodBias(word)) * ExponentScale(word);
}

// The machine: p is the predicate (the taps run while it is set), count the stack depth (0 = open).
bool p;
float count;
float total;  // the weights' x summed
vec3 rgb;     // the weighted colour sum
vec4 tap;     // the last tap read

// The guest's push of "the machine is open and flag is set" (setp_ne_push) and its pop (setp_pop).
void Push(float flag) {
  bool on = (count == 0.0) && (flag != 0.0);
  count = (on ? -1.0 : count) + 1.0;
  p = on;
}

void Pop() {
  float down = count - 1.0;
  p = down <= 0.0;
  count = p ? 0.0 : down;
}

// One of the taps c2..c12: read while open, flagged when c255.x is above its alpha, added when flagged.
void Step(vec2 offset, vec4 weight, inout float flag) {
  if (p) {
    tap = Tap(in_uv.xy + offset);
    flag = float(C255.x > tap.w);
  }
  Push(flag);
  if (p) {
    total = weight.x + total;
    rgb = fma(tap.rgb, weight.xyz, rgb);
  }
  Pop();
}

void main() {
  vec4 own = Tap(in_uv.xy);
  rgb = own.rgb;
  float flag = own.w;    // (r1.w: the guest's flag register, still holding the pixel's alpha until a tap is read)
  float flag12 = 0.0;    // (r0.x: the twelfth tap's flag)
  tap = vec4(0.0);
  total = in_uv.z;
  bool open_ = (C255.y == 0.0) && (-own.w >= 0.0);
  count = (open_ ? -1.0 : C255.y) + 1.0;
  p = open_;
  if (p) {
    tap = Tap(in_uv.xy + C1.xy);
    rgb = rgb * W16.x;
    flag = float(C255.x > tap.w);
    total = W16.x;
  }
  Push(flag);
  if (p) {
    total = W16.x + W17.x;
    rgb = fma(tap.rgb, W17.xyz, rgb);
  }
  Pop();
  Step(C2.xy, W18, flag);
  Step(C3.xy, W19, flag);
  Step(C4.xy, W20, flag);
  Step(C5.xy, W21, flag);
  Step(C6.xy, W22, flag);
  Step(C7.xy, W23, flag);
  Step(C8.xy, W24, flag);
  Step(C9.xy, W25, flag);
  Step(C10.xy, W26, flag);
  Step(C11.xy, W27, flag);
  Step(C12.xy, W28, flag12);
  vec4 result = vec4(0.0);
  if (p) {  // the blur: the sum times 1 / the weight total, alpha 1
    float inv_total = 1.0 / total;
    result = vec4(Mul0(inv_total, rgb.x), Mul0(inv_total, rgb.y), Mul0(inv_total, rgb.z), 1.0);
  }
  if (count == 1.0) result = vec4(rgb, flag);  // (the guest's inverted predicate: the machine never opened)
  XeOutputColor(result);
}
