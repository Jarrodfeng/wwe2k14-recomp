// Session 87 (stage 4): the vertex texture fetch of fetch constant 17 (the game's yFlashDefault reads its flash map per vertex).
// As xenos_vs_tex.glsl (fetch constant 16): the declarations make the swap require the translation to have fetch constant 17's
// 2D images and sampler. Include after xenos_vs.glsl. Our own code, following the SDK translator's tfetch for a vertex shader:
// a half-texel nudge of the coordinate from the texture's size, an explicit LOD from the fetch constant, the unsigned or signed
// image by the fetch constant's swizzled signs, the sign mode of each channel and the fetch constant's exponent adjust.

layout(set = 2, binding = 1017) uniform texture2DArray t_vertex17_unsigned;  // fetch constant 17, the unsigned image
layout(set = 2, binding = 1117) uniform texture2DArray t_vertex17_signed;    // fetch constant 17, the signed image
layout(set = 2, binding = 1217) uniform sampler s_vertex17;                  // fetch constant 17's sampler

#include "xenos_vs_texsign.glsl"

// Fetch constant 17 as a 2D texture with an explicit LOD (the fetch constant's), three channels.
vec3 XeVertexTexture17(vec2 uv) {
  uint size = xe_fetch.fetch_constants[26].x;
  precise float u = uv.x + 0.00146484375 / float(bitfieldExtract(size, 0, 13) + 1u);
  precise float v = uv.y + 0.00146484375 / float(bitfieldExtract(size, 13, 13) + 1u);
  uint signs = xe_system.vertex_texture_signs;
  uint sx = bitfieldExtract(signs, 8, 2), sy = bitfieldExtract(signs, 10, 2), sz = bitfieldExtract(signs, 12, 2);
  int word = int(xe_fetch.fetch_constants[26].z);
  precise float lod = float(bitfieldExtract(word, 12, 10)) * 0.03125;
  vec3 coordinate = vec3(u, v, 0.0);
  vec4 from_unsigned = vec4(0.0), from_signed = vec4(0.0);
  if (!(sx == 1u && sy == 1u && sz == 1u)) {
    from_unsigned = textureLod(sampler2DArray(t_vertex17_unsigned, s_vertex17), coordinate, lod);
  }
  if (sx == 1u || sy == 1u || sz == 1u) {
    from_signed = textureLod(sampler2DArray(t_vertex17_signed, s_vertex17), coordinate, lod);
  }
  float scale = ldexp(1.0, bitfieldExtract(word, 13, 6));
  precise vec3 color = vec3(XeVertexSign(sx, from_unsigned.x, from_signed.x), XeVertexSign(sy, from_unsigned.y, from_signed.y),
                            XeVertexSign(sz, from_unsigned.z, from_signed.z)) * scale;
  return color;
}
