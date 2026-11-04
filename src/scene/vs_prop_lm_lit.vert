#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yProp_DefaultLM (five interpolators) (guest vertex shader 7D652F24E7C76062; 8 / 8 / 0 draws a frame in
// Brock's entrance / Bryan's entrance / the match). Pixel side: F7FA89EF (bench census). Hand-written from our reading of the guest shader (see vs_prop.glsl for
// the skinning and the lighting); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
//
// As vs_prop_lit with the lighting-map coordinate added (vs_prop_lm; this declaration keeps the position in registers as x, z, y).
// Interpolators: 0 = (u, v, 0); 1 = the negated skinned normal; 2 = the camera (c72) less the world position; 3 = the lighting-map
// coordinate; 4 = the lit colour x vertex colour.
// Float constants: the whole block by number (kDynamic).

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#include "vs_prop.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_normal;
layout(location = 2) invariant out vec4 out_view;
layout(location = 3) invariant out vec4 out_lightmap;
layout(location = 4) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 8, 95);
  vec4 lit = fc.c[255];
  vec4 position = AvFetchPosition(at);
  vec4 bytes = PropBoneBytes(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColorAt(at + 6);
  vec2 halves = AvFetchHalves(at);
  vec4 weights = vec4(0.0, 0.0, 0.0, 1.0);

  vec3 world = PropSkinPosition(position, bytes, weights, lit);
  vec3 turned = PropSkinNormal(normal, bytes, weights, lit);

  vec4 p = vec4((lit.x == 0.0) ? world.z : lit.y, (lit.x == 0.0) ? world.x : lit.y, (lit.x == 0.0) ? world.y : lit.y,
                (lit.y == 0.0) ? world.y : lit.y);
  XeOutputPosition(vec4(AvDot4R(fc.c[56].zxyw, p), AvDot4R(fc.c[57].zxyw, p), AvDot4R(fc.c[58].zxyw, p), AvDot4R(fc.c[59].zxyw, p)));

  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  out_normal = vec4(-turned, 0.0);
  precise vec3 view = (-world) + fc.c[72].xyz;
  out_view = vec4(view, 0.0);
  precise float scale_u = AvMulX(fc.c[184].x, lit.w);
  precise float scale_v = AvMulX(fc.c[184].y, lit.w);
  precise vec2 lightmap = vec2(AvMulX(world.x, scale_u), AvMulX(world.z, scale_v)) + vec2(lit.w);
  out_lightmap = vec4(lightmap, 0.0, 0.0);
  vec3 lit_color = PropLitColor(turned.zxy, lit);
  out_color = vec4(AvMulX3(lit_color, color.rgb), AvMulX(PropAlpha(), color.w));
}
