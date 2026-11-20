// S105 A5c: the pixel bool 150 of the CAW logo layer (docs/research/s105/A5c.md item 3 [C]: sub_82C04DF0 0x82C096F8-0x82C0971C). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The yCh FX_LOGO fragments read `fx_bool(fc.Bools, 150) ? texture(textures[7]) : texture(textures[4])` (fx_skin.glsl). The game sets the bool per part: on when the part's diffuse sampler names texture index 0
// of the model's table (the part flag kGmDiffuseT0, Material::diffuse_t0). Bit 150 is word 4 of the 8 words of Bools (uvec4 Bools[2]: Bools[1].x), bit 22. Pure: no includes beyond the standard library.
#pragma once
#include <cstdint>
#include <cstring>

namespace rdr_scene {

inline constexpr uint32_t kLogoBoolBit = 150;
// Test only: 1 = bit 150 is never set (the logo layer is always slot 4); 2 = it is always set (the flag is ignored).
inline int g_logo_bool_plant = 0;

// `out` = the Bools constant's floats (vec4s vec4 = 4 * vec4s words), already holding the vertex stage's bits. Sets bit 150 for a part with `diffuse_t0`.
inline void SetLogoBool(float* out, uint32_t vec4s, bool diffuse_t0) {
  const bool on = g_logo_bool_plant == 2 ? true : (g_logo_bool_plant == 1 ? false : diffuse_t0);
  if (!on || vec4s < 2) return;
  const uint32_t word = kLogoBoolBit >> 5;   // 4
  uint32_t w;
  std::memcpy(&w, out + word, 4);
  w |= 1u << (kLogoBoolBit & 31);
  std::memcpy(out + word, &w, 4);
}

}  // namespace rdr_scene
