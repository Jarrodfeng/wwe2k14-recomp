// S102 lane U1: CPU decode of the block-compressed textures (BC1-BC5) for devices that cannot sample them.
// Mali, Xclipse and Adreno 6xx have no BC formats (docs\research\gpu_matrix_s93.md). The old (SDK) path transcoded; the owned renderer did not:
// CreateImage refused every BC texture by name (image_format_unsupported) and every picture made of one (the title card, the menus) was missing.
// The renderer makes such an image in a plain format instead (BC1/2/3 -> RGBA8, BC4 -> R8, BC5 -> RG8) and UpdateImage decodes each level on the
// way into the staging buffer. The values are the GPU's decode up to the usual 1-level rounding of the 565 palette.
// Std only (the unit test tools\bc\bc_decode_test.cpp includes this file and api.h only).
#pragma once
#include <cstddef>
#include <cstdint>

#include "api.h"

namespace rdr::bc {

inline bool IsBc(Format f) { return f >= Format::BC1 && f <= Format::BC5; }
// The plain format a BC format is decoded into (Undefined for anything else).
inline Format Substitute(Format f) {
  switch (f) {
    case Format::BC1:
    case Format::BC2:
    case Format::BC3: return Format::RGBA8;
    case Format::BC4: return Format::R8;
    case Format::BC5: return Format::RG8;
    default: return Format::Undefined;
  }
}
inline uint32_t DecodedTexelBytes(Format f) { return f == Format::BC4 ? 1u : f == Format::BC5 ? 2u : 4u; }
inline size_t BlockBytes(Format f) { return f == Format::BC1 || f == Format::BC4 ? 8 : 16; }
inline size_t EncodedBytes(Format f, uint32_t w, uint32_t h) { return size_t((w + 3) / 4) * ((h + 3) / 4) * BlockBytes(f); }
inline size_t DecodedBytes(Format f, uint32_t w, uint32_t h) { return size_t(w) * h * DecodedTexelBytes(f); }

namespace detail {
// 16 values of an 8-value-ramp channel (the BC3 alpha block, BC4, each BC5 channel), 8 bytes in.
inline void Ramp(const uint8_t* b, uint8_t out[16]) {
  const int a0 = b[0], a1 = b[1];
  int ramp[8] = {a0, a1};
  if (a0 > a1) {
    for (int k = 1; k < 7; ++k) ramp[k + 1] = ((7 - k) * a0 + k * a1) / 7;
  } else {
    for (int k = 1; k < 5; ++k) ramp[k + 1] = ((5 - k) * a0 + k * a1) / 5;
    ramp[6] = 0, ramp[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
  for (int p = 0; p < 16; ++p) out[p] = uint8_t(ramp[(bits >> (3 * p)) & 7]);
}
// The colour half (8 bytes) of BC1/2/3 -> 16 RGBA texels. `punch`: BC1's 3-colour mode (c0 <= c1) makes index 3 transparent black.
inline void Colour(const uint8_t* b, bool punch, uint8_t px[16][4]) {
  const uint32_t c0 = uint32_t(b[0] | b[1] << 8), c1 = uint32_t(b[2] | b[3] << 8);
  uint8_t pal[4][4];
  const auto expand = [](uint32_t c, uint8_t* o) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
    o[0] = uint8_t((r << 3) | (r >> 2)), o[1] = uint8_t((g << 2) | (g >> 4)), o[2] = uint8_t((bl << 3) | (bl >> 2)), o[3] = 255;
  };
  expand(c0, pal[0]);
  expand(c1, pal[1]);
  if (c0 > c1 || !punch) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
  }
  const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
  for (int p = 0; p < 16; ++p)
    for (int k = 0; k < 4; ++k) px[p][k] = pal[(idx >> (2 * p)) & 3][k];
}
}  // namespace detail

// Decodes one level (width x height texels; tightly packed encoded blocks in `src`, `src_bytes` long) into `dst` (DecodedBytes long). A source shorter
// than EncodedBytes decodes the blocks it has and leaves the rest zero (transparent black): returns false then. Edge blocks are cropped.
inline bool Decode(Format f, const uint8_t* src, size_t src_bytes, uint32_t w, uint32_t h, uint8_t* dst) {
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  const size_t bs = BlockBytes(f), tb = DecodedTexelBytes(f);
  const size_t have = src_bytes / bs;
  // A complete source writes every texel (edge blocks are cropped, never skipped): no zero fill then (the destination may be write-combined staging memory).
  if (have < size_t(bw) * bh)
    for (size_t i = 0; i < DecodedBytes(f, w, h); ++i) dst[i] = 0;
  bool complete = true;
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const size_t n = size_t(by) * bw + bx;
      if (n >= have) {
        complete = false;
        continue;
      }
      const uint8_t* b = src + n * bs;
      uint8_t px[16][4] = {};
      uint8_t ch[2][16] = {};
      switch (f) {
        case Format::BC1: detail::Colour(b, true, px); break;
        case Format::BC2: {
          detail::Colour(b + 8, false, px);
          for (int p = 0; p < 16; ++p) px[p][3] = uint8_t(((b[p / 2] >> (4 * (p & 1))) & 15) * 17);
          break;
        }
        case Format::BC3: {
          detail::Colour(b + 8, false, px);
          detail::Ramp(b, ch[0]);
          for (int p = 0; p < 16; ++p) px[p][3] = ch[0][p];
          break;
        }
        case Format::BC4: detail::Ramp(b, ch[0]); break;
        case Format::BC5: detail::Ramp(b, ch[0]), detail::Ramp(b + 8, ch[1]); break;
        default: return false;
      }
      for (int p = 0; p < 16; ++p) {
        const uint32_t x = bx * 4 + p % 4, y = by * 4 + p / 4;
        if (x >= w || y >= h) continue;
        uint8_t* o = dst + (size_t(y) * w + x) * tb;
        if (f == Format::BC4) {
          o[0] = ch[0][p];
        } else if (f == Format::BC5) {
          o[0] = ch[0][p], o[1] = ch[1][p];
        } else {
          o[0] = px[p][0], o[1] = px[p][1], o[2] = px[p][2], o[3] = px[p][3];
        }
      }
    }
  return complete;
}

}  // namespace rdr::bc
