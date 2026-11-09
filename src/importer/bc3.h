// Session 98 (Superstar Importer core): our own DXT5 (BC3) encoder, a resampler and the DDS writer for the roster renders
// (docs\formats\roster_images.md: SSFA / SSFB 512 x 512, SSFC 256 x 256, SD A / B squashed to 512 x 256; DXT5, one mip,
// a standard 128-byte header, not tiled). Std only. Colour endpoints: the block's bounding box on its principal axis
// (inset by 1/16), RGB565, 4-colour mode; alpha: min / max with the 8-value ramp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace importer::bc3 {

// Bilinear resample of RGBA8 (alpha-premultiplied while filtering, so cut-out edges keep no dark fringe).
inline std::vector<uint8_t> Resize(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh) {
  std::vector<uint8_t> out(size_t(dw) * dh * 4);
  if (sw <= 0 || sh <= 0 || src.size() < size_t(sw) * sh * 4) return out;
  if (sw == dw && sh == dh) return src;
  auto px = [&](int x, int y, int c) { return float(src[(size_t(y) * sw + x) * 4 + c]); };
  // a box pre-filter when shrinking a lot: average over the footprint (kept simple: sample a grid inside it)
  const int gx = std::max(1, sw / dw), gy = std::max(1, sh / dh);
  for (int y = 0; y < dh; ++y)
    for (int x = 0; x < dw; ++x) {
      float acc[4] = {0, 0, 0, 0}, wsum = 0;
      for (int jy = 0; jy < gy; ++jy)
        for (int jx = 0; jx < gx; ++jx) {
          const float fx = (x + (jx + 0.5f) / gx) * sw / dw - 0.5f, fy = (y + (jy + 0.5f) / gy) * sh / dh - 0.5f;
          const int x0 = std::clamp(int(std::floor(fx)), 0, sw - 1), y0 = std::clamp(int(std::floor(fy)), 0, sh - 1);
          const int x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
          const float tx = std::clamp(fx - std::floor(fx), 0.f, 1.f), ty = std::clamp(fy - std::floor(fy), 0.f, 1.f);
          const float w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
          const int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
          for (int k = 0; k < 4; ++k) {
            const float a = px(xs[k], ys[k], 3) / 255.f;
            for (int c = 0; c < 3; ++c) acc[c] += w[k] * px(xs[k], ys[k], c) * a;
            acc[3] += w[k] * a;
          }
          wsum += 1;
        }
      uint8_t* o = &out[(size_t(y) * dw + x) * 4];
      const float a = acc[3] / wsum;
      for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(a > 1e-6f ? acc[c] / wsum / a : 0.f, 0.f, 255.f) + 0.5f);
      o[3] = uint8_t(std::clamp(a * 255.f, 0.f, 255.f) + 0.5f);
    }
  return out;
}

namespace detail {
inline uint16_t To565(const float c[3]) {
  const int r = std::clamp(int(c[0] * 31.f / 255.f + 0.5f), 0, 31), g = std::clamp(int(c[1] * 63.f / 255.f + 0.5f), 0, 63),
            b = std::clamp(int(c[2] * 31.f / 255.f + 0.5f), 0, 31);
  return uint16_t(r << 11 | g << 5 | b);
}
inline void From565(uint16_t v, float c[3]) {
  c[0] = float((v >> 11) & 31) * 255.f / 31.f, c[1] = float((v >> 5) & 63) * 255.f / 63.f, c[2] = float(v & 31) * 255.f / 31.f;
}
inline void ColourBlock(const uint8_t px[16][4], uint8_t out[8]) {
  float mean[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i)
    for (int c = 0; c < 3; ++c) mean[c] += px[i][c] / 16.f;
  float cov[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    const float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
    cov[0] += d[0] * d[0], cov[1] += d[0] * d[1], cov[2] += d[0] * d[2], cov[3] += d[1] * d[1], cov[4] += d[1] * d[2], cov[5] += d[2] * d[2];
  }
  float axis[3] = {1, 1, 1};
  for (int it = 0; it < 8; ++it) {  // power iteration for the principal axis
    const float v[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                        cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1e-6f) break;
    for (int c = 0; c < 3; ++c) axis[c] = v[c] / n;
  }
  float lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < 16; ++i) {
    const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
    lo = std::min(lo, t), hi = std::max(hi, t);
  }
  const float inset = (hi - lo) / 16.f;
  lo += inset, hi -= inset;
  float c0[3], c1[3];
  for (int c = 0; c < 3; ++c) c0[c] = std::clamp(mean[c] + axis[c] * hi, 0.f, 255.f), c1[c] = std::clamp(mean[c] + axis[c] * lo, 0.f, 255.f);
  uint16_t e0 = To565(c0), e1 = To565(c1);
  if (e0 < e1) std::swap(e0, e1);
  uint32_t idx = 0;
  if (e0 != e1) {
    float p[4][3];
    From565(e0, p[0]), From565(e1, p[1]);
    for (int c = 0; c < 3; ++c) p[2][c] = (2 * p[0][c] + p[1][c]) / 3, p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      float bd = 1e30f;
      for (int k = 0; k < 4; ++k) {
        float d = 0;
        for (int c = 0; c < 3; ++c) d += (px[i][c] - p[k][c]) * (px[i][c] - p[k][c]);
        if (d < bd) bd = d, best = k;
      }
      idx |= uint32_t(best) << (2 * i);
    }
  }
  out[0] = uint8_t(e0), out[1] = uint8_t(e0 >> 8), out[2] = uint8_t(e1), out[3] = uint8_t(e1 >> 8);
  for (int k = 0; k < 4; ++k) out[4 + k] = uint8_t(idx >> (8 * k));
}
inline void AlphaBlock(const uint8_t px[16][4], uint8_t out[8]) {
  int lo = 255, hi = 0;
  for (int i = 0; i < 16; ++i) lo = std::min(lo, int(px[i][3])), hi = std::max(hi, int(px[i][3]));
  out[0] = uint8_t(hi), out[1] = uint8_t(lo);  // a0 > a1: the 8-value ramp (equal: every index gives a0)
  uint64_t bits = 0;
  if (hi > lo) {
    int ramp[8] = {hi, lo};
    for (int k = 1; k < 7; ++k) ramp[k + 1] = ((7 - k) * hi + k * lo) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0, bd = 1 << 30;
      for (int k = 0; k < 8; ++k) {
        const int d = std::abs(int(px[i][3]) - ramp[k]);
        if (d < bd) bd = d, best = k;
      }
      bits |= uint64_t(best) << (3 * i);
    }
  }
  for (int k = 0; k < 6; ++k) out[2 + k] = uint8_t(bits >> (8 * k));
}
}  // namespace detail

// RGBA8 (w, h multiples of 4) -> BC3 blocks.
inline std::vector<uint8_t> Encode(const std::vector<uint8_t>& rgba, int w, int h) {
  std::vector<uint8_t> out(size_t((w + 3) / 4) * ((h + 3) / 4) * 16);
  size_t o = 0;
  for (int by = 0; by < h; by += 4)
    for (int bx = 0; bx < w; bx += 4) {
      uint8_t px[16][4];
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const int sx = std::min(bx + x, w - 1), sy = std::min(by + y, h - 1);
          std::memcpy(px[y * 4 + x], &rgba[(size_t(sy) * w + sx) * 4], 4);
        }
      detail::AlphaBlock(px, &out[o]);
      detail::ColourBlock(px, &out[o + 8]);
      o += 16;
    }
  return out;
}

// A DDS file: 'DDS ', the 124-byte header (flags CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE = 0x81007, pixel format
// FOURCC 'DXT5', caps TEXTURE), then the blocks. The game's own renders measure 262,272 B (512 x 512) / 65,664 B (256 x 256).
inline std::vector<uint8_t> Dds(const std::vector<uint8_t>& rgba, int w, int h) {
  const std::vector<uint8_t> blocks = Encode(rgba, w, h);
  std::vector<uint8_t> d(128, 0);
  auto w32 = [&](size_t off, uint32_t v) { std::memcpy(&d[off], &v, 4); };
  std::memcpy(d.data(), "DDS ", 4);
  w32(4, 124), w32(8, 0x81007), w32(12, uint32_t(h)), w32(16, uint32_t(w)), w32(20, uint32_t(blocks.size()));
  // mip count 0 and no MIPMAPCOUNT flag: as the game's own renders (menuHD.pac SSFA 0100's header, read S98)
  w32(76, 32), w32(80, 4);  // pixel format: size, DDPF_FOURCC
  std::memcpy(&d[84], "DXT5", 4);
  w32(108, 0x1000);  // DDSCAPS_TEXTURE
  d.insert(d.end(), blocks.begin(), blocks.end());
  return d;
}

}  // namespace importer::bc3
