// Session 87 (stage 4): the vertex texture fetch of fetch constant 16 for our own arena vertex shaders that read the
// lighting map per vertex (the game's lightmapped materials with a vertex-side lookup). Include after xenos_vs.glsl, in a
// shader that uses it (the declarations below make the swap require the translation to have fetch constant 16's
// 2D images and sampler). Our own code, following the SDK translator's tfetch for a vertex shader: a half-texel nudge of
// the coordinate from the texture's size, an explicit LOD from the fetch constant, the unsigned or signed image by the
// fetch constant's swizzled signs, the sign mode of each channel (unsigned, signed, biased, gamma) and the fetch constant's
// exponent adjust.

layout(set = 2, binding = 1016) uniform texture2DArray t_vertex_unsigned;  // fetch constant 16, the unsigned image
layout(set = 2, binding = 1116) uniform texture2DArray t_vertex_signed;    // fetch constant 16, the signed image
layout(set = 2, binding = 1216) uniform sampler s_vertex;                  // fetch constant 16's sampler

#include "xenos_vs_texsign.glsl"

// Fetch constant 16 as a 2D texture with an explicit LOD (the fetch constant's), three channels.
vec3 XeVertexTexture16(vec2 uv) {
  uint size = xe_fetch.fetch_constants[24].z;
  precise float u = uv.x + 0.00146484375 / float(bitfieldExtract(size, 0, 13) + 1u);
  precise float v = uv.y + 0.00146484375 / float(bitfieldExtract(size, 13, 13) + 1u);
  uint signs = xe_system.vertex_texture_signs;
  uint sx = bitfieldExtract(signs, 0, 2), sy = bitfieldExtract(signs, 2, 2), sz = bitfieldExtract(signs, 4, 2);
  int word = int(xe_fetch.fetch_constants[25].x);
  precise float lod = float(bitfieldExtract(word, 12, 10)) * 0.03125;
  vec3 coordinate = vec3(u, v, 0.0);
  vec4 from_unsigned = vec4(0.0), from_signed = vec4(0.0);
  if (!(sx == 1u && sy == 1u && sz == 1u)) {
    from_unsigned = textureLod(sampler2DArray(t_vertex_unsigned, s_vertex), coordinate, lod);
  }
  if (sx == 1u || sy == 1u || sz == 1u) {
    from_signed = textureLod(sampler2DArray(t_vertex_signed, s_vertex), coordinate, lod);
  }
  float scale = ldexp(1.0, bitfieldExtract(word, 13, 6));
  precise vec3 color = vec3(XeVertexSign(sx, from_unsigned.x, from_signed.x), XeVertexSign(sy, from_unsigned.y, from_signed.y),
                            XeVertexSign(sz, from_unsigned.z, from_signed.z)) * scale;
  return color;
}

// Fetch constant 16 with its fourth channel too (the alpha: the same sign modes, bits 6-7 of the swizzled signs).
vec4 XeVertexTexture16Rgba(vec2 uv) {
  uint size = xe_fetch.fetch_constants[24].z;
  precise float u = uv.x + 0.00146484375 / float(bitfieldExtract(size, 0, 13) + 1u);
  precise float v = uv.y + 0.00146484375 / float(bitfieldExtract(size, 13, 13) + 1u);
  uint signs = xe_system.vertex_texture_signs;
  uint sx = bitfieldExtract(signs, 0, 2), sy = bitfieldExtract(signs, 2, 2), sz = bitfieldExtract(signs, 4, 2);
  uint sw = bitfieldExtract(signs, 6, 2);
  int word = int(xe_fetch.fetch_constants[25].x);
  precise float lod = float(bitfieldExtract(word, 12, 10)) * 0.03125;
  vec3 coordinate = vec3(u, v, 0.0);
  vec4 from_unsigned = vec4(0.0), from_signed = vec4(0.0);
  if (!(sx == 1u && sy == 1u && sz == 1u && sw == 1u)) {
    from_unsigned = textureLod(sampler2DArray(t_vertex_unsigned, s_vertex), coordinate, lod);
  }
  if (sx == 1u || sy == 1u || sz == 1u || sw == 1u) {
    from_signed = textureLod(sampler2DArray(t_vertex_signed, s_vertex), coordinate, lod);
  }
  float scale = ldexp(1.0, bitfieldExtract(word, 13, 6));
  precise vec4 color = vec4(XeVertexSign(sx, from_unsigned.x, from_signed.x), XeVertexSign(sy, from_unsigned.y, from_signed.y),
                            XeVertexSign(sz, from_unsigned.z, from_signed.z), XeVertexSign(sw, from_unsigned.w, from_signed.w)) * scale;
  return color;
}
