#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the sweat maps' copy step (the game's
// yLiquidMap, guest pixel shader F235532B75570EAF; it walks the sweat maps' small mip levels, which the skin and gear
// read), through the shader swap (src\scene_swap.h): the texture's colour at the first interpolator's zw, alpha 1.
// Unlike the materials, the fetch constant's LOD bias and exponent adjust are kept, read as the SDK translator reads
// them: the bias is what picks the source level of the chain (dropping it changed the wrestlers' sweat in shots, runs
// 5517 / 5518). No float constants.

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;

layout(set = 3, binding = 1000) uniform texture2DArray t_source;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_source;

layout(location = 0) in vec4 in_uv;  // zw: the source's coordinate

void main() {
  int word = int(xe_fetch.fetch_constants[1].x);  // fetch constant 0's LOD bias and exponent adjust fields
  float lod = float(bitfieldExtract(word, 12, 10)) * (1.0 / 32.0);
  float scale = ldexp(1.0, bitfieldExtract(word, 13, 6));
  vec3 source = textureLod(sampler2DArray(t_source, s_source), vec3(in_uv.zw, 0.0), lod).rgb;
  XeOutputColor(vec4(source * scale, 1.0));
}
