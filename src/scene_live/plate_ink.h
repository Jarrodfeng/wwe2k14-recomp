// S110 PLATEINK: how much ink a belt plate holds (docs\research\s110\PLATENAME.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: std + ../assets/asset_model.h. The logged "below-255 share" (assets::AlphaClearShare) counts texels with alpha < 255, so a plate whose letters are black ink at alpha <= 108 reads 1.000, the same
// as an empty one. This counts what the share cannot tell: texels with alpha > 0, and the highest alpha. BC3 only (the plate's format); any other format returns {0, 0}.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "../assets/asset_model.h"

namespace rdr_scene::live::beltplate {

struct Ink {
  uint32_t texels = 0;      // texels with alpha > 0
  uint32_t max_alpha = 0;   // the highest alpha of any texel
};

// The alpha of each BC3 block decoded exactly as assets::AlphaClearShare (src\assets\jboy.h) does.
inline Ink PlateInk(const assets::Texture& t) {
  Ink ink;
  if (t.format != assets::TexFormat::kBC3) return ink;
  const size_t blocks = size_t((std::max(t.width, 1u) + 3) / 4) * ((std::max(t.height, 1u) + 3) / 4);
  if (blocks == 0 || t.data.size() < blocks * 16) return ink;
  for (size_t b = 0; b < blocks; ++b) {
    const uint8_t* q = &t.data[b * 16];
    const unsigned a0 = q[0], a1 = q[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(q[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) {
      const unsigned ix = unsigned((bits >> (3 * i)) & 7);
      unsigned v;
      if (ix == 0) v = a0;
      else if (ix == 1) v = a1;
      else if (a0 > a1) v = ((8 - ix) * a0 + (ix - 1) * a1) / 7;
      else if (ix == 6) v = 0;
      else if (ix == 7) v = 255;
      else v = ((6 - ix) * a0 + (ix - 1) * a1) / 5;
      if (v > 0) ++ink.texels;
      ink.max_alpha = std::max<uint32_t>(ink.max_alpha, v);
    }
  }
  return ink;
}

}  // namespace rdr_scene::live::beltplate
