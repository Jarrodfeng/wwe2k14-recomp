#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the game's yProp_PSMLM (guest vertex shader FE6B770B3CC9FA15; 1 / 1 / 0 draws a frame in
// Brock's entrance / Bryan's entrance / the match). Pixel side: unknown. Hand-written from our reading of the guest shader (see vs_prop.glsl for
// the skinning, the lighting and the shadow-map coordinate); checked bit for bit against the SDK's translation by tools/shaders/harness/vs_harness.py.
//
// vs_prop_shadow with the lighting-map coordinate added (the world x and z x c184 x 1/2, plus 1/2). Interpolators: 0 = (u, v);
// 1 = (world x x c207.x, world z x c207.y); 2 = the shadow-map coordinate; 3 = zeros; 4 = (-|w| > 0) three times, with w the shadow
// projection's w; 5 = the lighting-map coordinate; 6 = the lit colour x vertex colour.
// Float constants: the whole block by number (kDynamic).

#include "xenos_vs_math.glsl"
#include "xenos_vs_skin.glsl"
#include "vs_prop.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_world;
layout(location = 2) invariant out vec4 out_shadow;
layout(location = 3) invariant out vec4 out_unused;
layout(location = 4) invariant out vec4 out_flag;
layout(location = 5) invariant out vec4 out_lightmap;
layout(location = 6) invariant out vec4 out_color;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 8, 95);
  vec4 lit = fc.c[255];
  vec4 position = AvFetchPosition(at);
  vec4 bytes = PropBoneBytes(at);
  vec3 normal = AvFetchNormal(at);
  vec4 color = AvFetchColorAt(at + 6);
  vec2 halves = AvFetchHalves(at);
  vec4 weights = vec4(0.0, 0.0, 0.0, 1.0);  // (fixed by the vertex declaration)

  vec3 world = PropSkinPosition(position, bytes, weights, lit);
  vec3 turned = PropSkinNormal(normal, bytes, weights, lit);

  vec4 p = vec4((lit.x == 0.0) ? world.z : lit.y, (lit.x == 0.0) ? world.x : lit.y, (lit.x == 0.0) ? world.y : lit.y,
                (lit.y == 0.0) ? world.y : lit.y);
  vec4 clip = vec4(AvDot4R(fc.c[56].zxyw, p), AvDot4R(fc.c[57].zxyw, p), AvDot4R(fc.c[58].zxyw, p), AvDot4R(fc.c[59].zxyw, p));
  XeOutputPosition(clip);

  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  out_world = vec4(AvMulX(world.x, fc.c[207].x), AvMulX(world.z, fc.c[207].y), 0.0, 0.0);
  vec4 shadow = PropShadowCoordinate(p, clip.w, lit);
  out_shadow = vec4(shadow.xyz, 0.0);
  float faces = ((-abs(shadow.w)) > lit.x) ? 1.0 : 0.0;
  out_flag = vec4(faces, faces, faces, 0.0);
  precise float scale_u = AvMulX(fc.c[184].x, lit.w);
  precise float scale_v = AvMulX(fc.c[184].y, lit.w);
  precise vec2 lightmap = vec2(AvMulX(scale_u, world.x), AvMulX(scale_v, world.z)) + vec2(lit.w);
  out_lightmap = vec4(lightmap, 0.0, 0.0);
  vec3 lit_color = PropLitColor(turned.zxy, lit);
  out_color = vec4(AvMulX3(lit_color, color.rgb), AvMulX(PropAlpha(), color.w));
}
