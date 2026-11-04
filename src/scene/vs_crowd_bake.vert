#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_vs.glsl"
// Session 87 (stage 4, gpu_native_vs_swap): our own vertex shader for the quad that draws the crowd into its atlas (the game's
// yAudience atlas copy; guest vertex shader 5344BF064B17B7C2). Pixel side: the crowd atlas copy pixel shader 7ABDFEFBCADB4BF9.
// Hand-written from our reading of the guest shader; checked bit for bit against the SDK's translation by
// tools/shaders/harness/vs_harness.py. (Its draws are not in the bench census table; the logs of the integrator's runs 129-135 name the
// vertex shader as one our own crowd program does not take: "kept the game's draws".)
//
// A 4-dword vertex: position xyz (3 floats), a texture coordinate (2 halves at dword 3). The position goes through the model rows c76 / c77
// / c78 (a 4-term dot of (z, x, y, 1) against each row), the lighting map (the vertex texture of fetch constant 16, rgba) is read at the
// world x and the row-78 term scaled by c255.x and c184.xy (+ c255.x), and the clip position is the world position (z, x, y, 1) through
// the view-projection rows c56..c59. Interpolators: 0 = (u, v, 0, 0); 1 = the lighting-map colour x c184.z (rgba).
// Float constants (set 1 binding 1), packed: 56 57 58 59 76 77 78 184 255.

layout(set = 1, binding = 1, std140) uniform XeFloatConstants { vec4 c[9]; } fc;
#define C56 fc.c[0]
#define C57 fc.c[1]
#define C58 fc.c[2]
#define C59 fc.c[3]
#define C76 fc.c[4]
#define C77 fc.c[5]
#define C78 fc.c[6]
#define C184 fc.c[7]
#define C255 fc.c[8]

#include "xenos_vs_math.glsl"
#include "xenos_vs_tex.glsl"

layout(location = 0) invariant out vec4 out_uv;
layout(location = 1) invariant out vec4 out_map;

void main() {
  uint index = XeGuestVertexIndex();
  int at = XeVertexAddress(index, 4, 95);
  vec3 p = vec3(XeFetchFloat(at), XeFetchFloat(at + 1), XeFetchFloat(at + 2));
  vec2 halves = unpackHalf2x16(XeFetchDword(at + 3));

  vec4 q = vec4(p.z, p.x, p.y, 1.0);
  float world_x = AvDot4(C76.zxyw, q);   // row 76
  float world_z = AvDot4(C78.zxyw, q);   // row 78
  float world_y = AvDot4(C77.zxyw, q);   // row 77
  float index_value = float(index);
  float one = (abs(index_value) >= abs(index_value)) ? 1.0 : 0.0;  // (1 for any vertex)

  precise float half_x = world_x * C255.x;
  precise float half_z = world_z * C255.x;
  vec2 map_uv = vec2(fma(half_x, C184.x, C255.x), fma(half_z, C184.y, C255.x));
  vec4 map = XeVertexTexture16Rgba(map_uv);

  vec4 w = vec4(world_z, world_x, world_y, one);
  XeOutputPosition(vec4(AvDot4(C56.zxyw, w), AvDot4(C57.zxyw, w), AvDot4(C58.zxyw, w), AvDot4(C59.zxyw, w)));
  out_uv = vec4(halves.y, halves.x, 0.0, 0.0);
  precise vec4 scaled = map * C184.z;
  out_map = scaled;
}
