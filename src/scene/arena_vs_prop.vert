#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena props' plain material, the game's
// yProp_Default (guest vertex shader 6DC7C61EB5985AE8, a vertex-declaration copy of base shader 99E1F345...; 1 draw a frame
// in each scene). Pixel side: arena_prop.frag (guest pixel shader 468C06E826088769). Written from our reading of the guest
// microcode; checked against the translation by tools\shaders\harness\vs_harness.py.
//
// As arena_vs_bg.vert without the compare outputs, and the ambient blend factor fma(normal against c180, c255.x, c255.x).
// Interpolators: 0 = texture coordinate (xy); 1 = colour (rgb = vertex colour x light, a = vertex alpha x c185.x x c189.w x c190.w).
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 74 76 77 78 180 181 182 185
// 189 190 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[23]; } fc;
#define C0    fc.c[0]
#define C1    fc.c[1]
#define C2    fc.c[2]
#define C3    fc.c[3]
#define C8    fc.c[4]
#define C9    fc.c[5]
#define C10   fc.c[6]
#define C11   fc.c[7]
#define C56   fc.c[8]
#define C57   fc.c[9]
#define C58   fc.c[10]
#define C59   fc.c[11]
#define C74   fc.c[12]
#define C76   fc.c[13]
#define C77   fc.c[14]
#define C78   fc.c[15]
#define C180  fc.c[16]
#define C181  fc.c[17]
#define C182  fc.c[18]
#define C185  fc.c[19]
#define C189  fc.c[20]
#define C190  fc.c[21]
#define C255  fc.c[22]
#include "arena_vs.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index);
  vec4 position = AvFetchPosition(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColor(at);
  vec2 halves = AvFetchHalves(at);

  XeOutputPosition(AvClip(AvWorld(position), AvOne(float(index))));
  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  vec3 nw = AvWorldNormal(normal);
  float facing = AvFma(AvUpDot(nw), C255.x, C255.x);
  vec3 lit = AvLit(AvLightTerms(nw), AvAmbient(facing));
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise vec4 shaded = vec4(lit, alpha_scale) * color;
  out_color = shaded;
}
