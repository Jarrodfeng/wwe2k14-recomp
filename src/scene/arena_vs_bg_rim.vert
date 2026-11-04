#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the arena background's plain material with an edge
// fade, the game's yBG_Default (guest vertex shader 3E4670E18A977431, a vertex-declaration copy of base shader
// 58A626F5AB36DDDE; 6 / 3 draws a frame in Brock's / Bryan's entrance). Pixel side: arena_bg.frag (guest pixel shader
// F335375CE1CC39E4). Written from our reading of the guest microcode; checked against the translation by
// tools\shaders\harness\vs_harness.py.
//
// As arena_vs_bg.vert, with the vertex alpha faded towards the surface's edge as seen from the camera: the normalised
// normal against the normalised view vector (c72 less the world position; the guest's reciprocal roots, a zero length
// giving infinity as it does), |dot| x c254.z + c254.y floored at c255.x, then c254.w less the square of (c254.w less
// that): alpha x that. The ambient blend factor is (normal against c180) x c254.x + c254.x. Interpolators: 0 = texture
// coordinate (xy); 1 = zeros; 2 = (-|normal against c180| > c255.x) in xyz (compiler constants); 3 = colour (rgb =
// vertex colour x light, a = vertex alpha x c185.x x c189.w x c190.w x the edge fade).
// Float constants (set 1 binding 1, packed in register order): 0 1 2 3 8 9 10 11 56 57 58 59 72 74 76 77 78 180 181 182
// 185 189 190 254 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[25]; } fc;
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
#define C72   fc.c[12]
#define C74   fc.c[13]
#define C76   fc.c[14]
#define C77   fc.c[15]
#define C78   fc.c[16]
#define C180  fc.c[17]
#define C181  fc.c[18]
#define C182  fc.c[19]
#define C185  fc.c[20]
#define C189  fc.c[21]
#define C190  fc.c[22]
#define C254  fc.c[23]
#define C255  fc.c[24]
#include "arena_vs.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_one;
layout(location = 2) invariant out vec4 out_two;
layout(location = 3) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index);
  vec4 position = AvFetchPosition(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColor(at);
  vec2 halves = AvFetchHalves(at);

  // The guest's position w is "|r0.x| >= 0" with r0.x the world x by then.
  vec3 world = AvWorld(position);
  XeOutputPosition(AvClip(world, AvOne(world.x)));

  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  out_one = vec4(0.0);

  vec3 nw = AvWorldNormal(normal);
  float up = AvUpDot(nw);
  float cmp = (-abs(up) > C255.x) ? 1.0 : 0.0;
  out_two = vec4(cmp, cmp, cmp, 0.0);

  // The edge fade: the normal and the view vector normalised (the guest's 1 / sqrt|x|), dotted in the order z, y, x. The reciprocal
  // roots can be infinite, so the translation keeps the guest's multiply rule from here on (AvMulX).
  precise vec3 view = (-world) + C72.xyz;
  vec3 turned = vec3(nw.z, nw.x, nw.y);
  float normal_length2 = AvDot3(-turned, -turned);
  float inv_normal = inversesqrt(abs(normal_length2));
  precise float view_length2 = AvDot3(view.zxy, view.zxy);
  float inv_view = inversesqrt(abs(view_length2));
  vec3 view_unit = AvMulX3(vec3(view.x, view.z, view.y), vec3(inv_view));
  float normal_y = AvMulX(inv_normal, -nw.y);
  float normal_z = AvMulX(inv_normal, -nw.z);
  float view_x_scaled = AvMulX(view_unit.x, inv_normal);
  float cosine = AvDot3R(vec3(view_unit.y, view_unit.z, view_x_scaled), vec3(normal_z, normal_y, -nw.x));
  precise float cosine_scaled = AvMulX(abs(cosine), C254.z);
  precise float rim = cosine_scaled + C254.y;
  float rim_floored = rim >= C255.x ? rim : C255.x;
  precise float rest = C254.w - rim_floored;
  precise float edge = AvMulX(-abs(rest), abs(rest)) + C254.w;

  precise float up_scaled = up * C254.x;
  precise float facing = up_scaled + C254.x;
  vec4 lights = AvLightTerms(nw);
  precise vec3 span = C182.xyz - C181.xyz;
  precise vec3 blended = AvMulX3(span, vec3(facing)) + C181.xyz;
  vec3 ambient = AvMulX3(blended, C189.xyz);
  precise vec3 sum = lights.y * C3.xyz;
  sum = AvFma(vec3(lights.z), C2.xyz, sum);
  sum = AvFma(vec3(lights.w), C1.xyz, sum);
  sum = AvFma(vec3(lights.x), C0.xyz, sum);
  precise vec3 lit = AvMulX3(sum, C190.xyz) + ambient;
  vec3 rgb = AvMulX3(lit, color.rgb);
  precise float alpha_scale = C185.x * (C189.w * C190.w);
  precise float alpha = alpha_scale * color.w;
  float faded = AvMulX(alpha, edge);
  out_color = vec4(rgb, faded);
}
