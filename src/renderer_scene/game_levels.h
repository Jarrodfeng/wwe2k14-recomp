// S111 MIPLEVELS (docs\research\s111\MIPLEVELS.md): the level count the game's texture loader gives a file texture, and the levels ours uploads to match it.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only: no renderer, no game or SDK includes. FrontEnd::AddModelImpl (front_end.cpp) and the
// offline unit (tools\scene\game_levels_unit.cpp) run THESE.
// The game: a file texture is made by its D3DX-style loader (sub_82BD4360 -> sub_82CCCF30) with the level count held in the global word 0x83629CA8, which each
// loader sets around its build. The loader creates min(count, the size's whole chain) levels (count 0 = the whole chain; sub_82CCC488), copies the file's levels
// into them, and filters the levels the file lacks from the file's last one (sub_82CCCB08; its filter argument is D3DX's default, a box). So a texture can end with FEWER
// levels than its file (a whole-chain file built at 5) or MORE (a 4-level body normal map built at 5).
// A texture's count comes from its loader: assets::Texture::game_levels (0 = not set: the file's own chain is uploaded, the old behaviour).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "../assets/asset_model.h"

namespace rdr_scene::game_levels {

inline size_t LevelBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

// The size's whole chain (2048 x 1024 -> 12).
inline uint32_t WholeChain(uint32_t w, uint32_t h) {
  uint32_t mx = std::max(w, h), n = 1;
  while (mx > 1) mx >>= 1, ++n;
  return n;
}

// The levels the game's loader creates for a w x h texture asked for `count` (0 = the whole chain; above the chain = the chain).
inline uint32_t GameLevels(uint32_t count, uint32_t w, uint32_t h) {
  const uint32_t chain = WholeChain(w, h);
  return count == 0 || count > chain ? chain : count;
}

namespace detail {

inline int Expand5(uint32_t v) { return int(v * 255 + 15) / 31; }
inline int Expand6(uint32_t v) { return int(v * 255 + 31) / 63; }
inline uint16_t Pack565(int r, int g, int b) { return uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)); }

// The 4 palette colours of a colour block (RGBA). `three` = BC1's c0 <= c1 mode (index 3 transparent); BC2 / BC3 colour blocks always use 4 colours.
inline void Palette(uint16_t c0, uint16_t c1, bool three, int p[4][4]) {
  const uint16_t c[2] = {c0, c1};
  for (int i = 0; i < 2; ++i) p[i][0] = Expand5((c[i] >> 11) & 31), p[i][1] = Expand6((c[i] >> 5) & 63), p[i][2] = Expand5(c[i] & 31), p[i][3] = 255;
  if (!three) {
    for (int k = 0; k < 3; ++k) p[2][k] = (2 * p[0][k] + p[1][k] + 1) / 3, p[3][k] = (p[0][k] + 2 * p[1][k] + 1) / 3;
    p[2][3] = p[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) p[2][k] = (p[0][k] + p[1][k] + 1) / 2, p[3][k] = 0;
    p[2][3] = 255, p[3][3] = 0;
  }
}

inline void DecodeColourBlock(const uint8_t* b, bool bc1, uint8_t out[16][4]) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
  const uint32_t idx = b[4] | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
  int p[4][4];
  Palette(c0, c1, bc1 && c0 <= c1, p);
  for (int i = 0; i < 16; ++i)
    for (int k = 0; k < 4; ++k) out[i][k] = uint8_t(p[(idx >> (2 * i)) & 3][k]);
}

// One colour block from 16 RGBA texels. `bc1` with a texel under alpha 128: BC1's 3-colour mode, those texels index 3 (transparent).
inline void EncodeColourBlock(const uint8_t px[16][4], bool bc1, uint8_t* o) {
  bool cut = false;
  int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    if (bc1 && px[i][3] < 128) { cut = true; continue; }
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], int(px[i][k])), hi[k] = std::max(hi[k], int(px[i][k]));
  }
  if (lo[0] > hi[0]) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;   // every texel cut
  for (int k = 0; k < 3; ++k) {
    const int inset = (hi[k] - lo[k]) / 16;
    lo[k] += inset, hi[k] -= inset;
  }
  uint16_t c0 = Pack565(hi[0], hi[1], hi[2]), c1 = Pack565(lo[0], lo[1], lo[2]);
  if (cut) {
    if (c0 > c1) std::swap(c0, c1);   // c0 <= c1: the 3-colour mode
  } else if (c0 < c1) {
    std::swap(c0, c1);                // c0 > c1 (or equal: a flat block, every index 0)
  }
  int p[4][4];
  Palette(c0, c1, bc1 && c0 <= c1, p);
  const int colours = (bc1 && c0 <= c1) ? 3 : 4;
  uint32_t idx = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0;
    if (cut && px[i][3] < 128) {
      best = 3;
    } else {
      int bd = 1 << 30;
      for (int q = 0; q < colours; ++q) {
        int d = 0;
        for (int k = 0; k < 3; ++k) d += (int(px[i][k]) - p[q][k]) * (int(px[i][k]) - p[q][k]);
        if (d < bd) bd = d, best = q;
      }
    }
    idx |= uint32_t(best) << (2 * i);
  }
  o[0] = uint8_t(c0), o[1] = uint8_t(c0 >> 8), o[2] = uint8_t(c1), o[3] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) o[4 + i] = uint8_t(idx >> (8 * i));
}

inline void DecodeAlpha3(const uint8_t* b, uint8_t out[16][4]) {   // BC3's alpha block
  const int a0 = b[0], a1 = b[1];
  int v[8] = {a0, a1};
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) v[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
  } else {
    for (int i = 1; i < 5; ++i) v[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
    v[6] = 0, v[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i][3] = uint8_t(v[(bits >> (3 * i)) & 7]);
}

inline void EncodeAlpha3(const uint8_t px[16][4], uint8_t* o) {
  int lo = 255, hi = 0;
  for (int i = 0; i < 16; ++i) lo = std::min(lo, int(px[i][3])), hi = std::max(hi, int(px[i][3]));
  o[0] = uint8_t(hi), o[1] = uint8_t(lo);
  uint64_t bits = 0;
  if (hi > lo) {   // the 8-value mode (a0 > a1)
    int v[8] = {hi, lo};
    for (int i = 1; i < 7; ++i) v[i + 1] = ((7 - i) * hi + i * lo + 3) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0, bd = 1 << 30;
      for (int q = 0; q < 8; ++q) {
        const int d = std::abs(int(px[i][3]) - v[q]);
        if (d < bd) bd = d, best = q;
      }
      bits |= uint64_t(best) << (3 * i);
    }
  }
  for (int i = 0; i < 6; ++i) o[2 + i] = uint8_t(bits >> (8 * i));
}

inline void DecodeAlpha2(const uint8_t* b, uint8_t out[16][4]) {   // BC2's explicit 4-bit alpha
  for (int i = 0; i < 16; ++i) out[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
}

inline void EncodeAlpha2(const uint8_t px[16][4], uint8_t* o) {
  for (int i = 0; i < 8; ++i) o[i] = 0;
  for (int i = 0; i < 16; ++i) o[i / 2] |= uint8_t(((int(px[i][3]) * 15 + 127) / 255) << (4 * (i & 1)));
}

// One level in `f` -> RGBA8 in the format's own channel order (re-encoded into the same format, so the order needs no naming).
inline void Decode(assets::TexFormat f, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
  rgba.assign(size_t(w) * h * 4, 0);
  if (f == assets::TexFormat::kBC1 || f == assets::TexFormat::kBC2 || f == assets::TexFormat::kBC3) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = f == assets::TexFormat::kBC1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; ++by)
      for (uint32_t bx = 0; bx < bw; ++bx) {
        const uint8_t* b = src + (size_t(by) * bw + bx) * bs;
        uint8_t t[16][4];
        DecodeColourBlock(b + (bs - 8), f == assets::TexFormat::kBC1, t);
        if (f == assets::TexFormat::kBC2) DecodeAlpha2(b, t);
        if (f == assets::TexFormat::kBC3) DecodeAlpha3(b, t);
        for (uint32_t y = 0; y < 4; ++y)
          for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t X = bx * 4 + x, Y = by * 4 + y;
            if (X < w && Y < h) std::copy(t[4 * y + x], t[4 * y + x] + 4, &rgba[(size_t(Y) * w + X) * 4]);
          }
      }
  } else if (f == assets::TexFormat::kB4G4R4A4) {
    for (size_t k = 0; k < size_t(w) * h; ++k) {
      const uint16_t px = uint16_t(src[2 * k] | (src[2 * k + 1] << 8));
      for (int c = 0; c < 4; ++c) rgba[4 * k + size_t(c)] = uint8_t(((px >> (4 * c)) & 15) * 17);
    }
  } else {
    std::copy(src, src + size_t(w) * h * 4, rgba.begin());
  }
}

inline void Encode(assets::TexFormat f, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, uint8_t* out) {
  if (f == assets::TexFormat::kBC1 || f == assets::TexFormat::kBC2 || f == assets::TexFormat::kBC3) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = f == assets::TexFormat::kBC1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; ++by)
      for (uint32_t bx = 0; bx < bw; ++bx) {
        uint8_t t[16][4];
        for (uint32_t y = 0; y < 4; ++y)
          for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t X = std::min(bx * 4 + x, w - 1), Y = std::min(by * 4 + y, h - 1);   // a level under 4 wide / high repeats its edge
            std::copy(&rgba[(size_t(Y) * w + X) * 4], &rgba[(size_t(Y) * w + X) * 4] + 4, t[4 * y + x]);
          }
        uint8_t* o = out + (size_t(by) * bw + bx) * bs;
        if (f == assets::TexFormat::kBC2) EncodeAlpha2(t, o);
        if (f == assets::TexFormat::kBC3) EncodeAlpha3(t, o);
        EncodeColourBlock(t, f == assets::TexFormat::kBC1, o + (bs - 8));
      }
  } else if (f == assets::TexFormat::kB4G4R4A4) {
    for (size_t k = 0; k < size_t(w) * h; ++k) {
      uint16_t px = 0;
      for (int c = 0; c < 4; ++c) px = uint16_t(px | (((int(rgba[4 * k + size_t(c)]) * 15 + 127) / 255) << (4 * c)));
      out[2 * k] = uint8_t(px), out[2 * k + 1] = uint8_t(px >> 8);
    }
  } else {
    std::copy(rgba.begin(), rgba.begin() + std::ptrdiff_t(size_t(w) * h * 4), out);
  }
}

// The 2 x 2 box of a sw x sh RGBA8 level into its next level (a 1-wide / 1-high side repeats its edge texel).
inline void Box(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, std::vector<uint8_t>& dst) {
  const uint32_t dw = std::max(1u, sw >> 1), dh = std::max(1u, sh >> 1);
  dst.assign(size_t(dw) * dh * 4, 0);
  for (uint32_t y = 0; y < dh; ++y)
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = std::min(2 * x, sw - 1), x1 = std::min(2 * x + 1, sw - 1), y0 = std::min(2 * y, sh - 1), y1 = std::min(2 * y + 1, sh - 1);
      for (int c = 0; c < 4; ++c) {
        const int s = src[(size_t(y0) * sw + x0) * 4 + size_t(c)] + src[(size_t(y0) * sw + x1) * 4 + size_t(c)] + src[(size_t(y1) * sw + x0) * 4 + size_t(c)] +
                      src[(size_t(y1) * sw + x1) * 4 + size_t(c)];
        dst[(size_t(y) * dw + x) * 4 + size_t(c)] = uint8_t((s + 2) / 4);
      }
    }
}

}  // namespace detail

namespace detail {

// One face's chain: the `have` whole levels of a w x h texture at `src`, cut or grown to `want` levels (each missing level the box of the one before it, re-encoded in `f`),
// appended to `out`. `have` and `want` >= 1.
inline void Chain(assets::TexFormat f, uint32_t w, uint32_t h, const uint8_t* src, uint32_t have, uint32_t want, std::vector<uint8_t>& out) {
  const uint32_t kept = std::min(have, want);
  size_t keep = 0;
  for (uint32_t i = 0; i < kept; ++i) keep += LevelBytes(f, std::max(1u, w >> i), std::max(1u, h >> i));
  const size_t base = out.size();
  out.insert(out.end(), src, src + std::ptrdiff_t(keep));
  std::vector<uint8_t> prev, next;
  size_t at = base + keep - LevelBytes(f, std::max(1u, w >> (kept - 1)), std::max(1u, h >> (kept - 1)));
  for (uint32_t i = kept; i < want; ++i) {
    const uint32_t pw = std::max(1u, w >> (i - 1)), ph = std::max(1u, h >> (i - 1));
    const uint32_t nw = std::max(1u, w >> i), nh = std::max(1u, h >> i);
    Decode(f, out.data() + at, pw, ph, prev);
    Box(prev, pw, ph, next);
    at = out.size();
    out.resize(at + LevelBytes(f, nw, nh));
    Encode(f, next, nw, nh, out.data() + at);
  }
}

}  // namespace detail

// True when `t` is to be uploaded as `out` instead: `t.game_levels` is set and the game's level count differs from the file's whole levels. `out` = `t` with
// `mips` = the game's count and `data` cut to those levels, or extended by the missing ones (each the box of the one before it, re-encoded in the file's format).
// S111 CUBELEVELS (docs\research\s111\CUBELEVELS.md): a cube (six square faces, face-major as dds.h ParseDds lays them) takes the same count, every face alike: the
// game's wrapper create hands the count word to its cube create as to its 2D one (sub_82BD4360 kind 1 -> sub_82CCD778), the creator clamps it the same (sub_82CCC488)
// and box-fills each face's missing levels (sub_82CCCB08 over six faces).
// False (and `out` untouched) for a texture without a count, or one MakeTexture refuses anyway (its refusal then reads the file as before; a cube without six whole chains).
inline bool Relevel(const assets::Texture& t, assets::Texture& out) {
  const bool cube = t.faces == 6 && t.width == t.height;
  if (t.game_levels == 0 || (t.faces != 1 && !cube) || t.width == 0 || t.height == 0 || t.format == assets::TexFormat::kUnknown) return false;
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if ((bc && ((t.width % 4) || (t.height % 4))) || t.width > 16384 || t.height > 16384) return false;
  const uint32_t chain = WholeChain(t.width, t.height), listed = std::min(std::max(1u, t.mips), chain);
  const uint32_t faces = cube ? 6u : 1u;
  const size_t room = t.data.size() / faces;
  uint32_t have = 0;
  size_t bytes = 0;
  for (uint32_t i = 0; i < listed; ++i) {   // one face's whole levels (MakeTexture's `fit`)
    const size_t sz = LevelBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (bytes + sz > room) break;
    bytes += sz, ++have;
  }
  if (cube && have != listed) return false;   // the faces lie at one whole chain each (MakeTexture refuses a cube without them too)
  const uint32_t want = GameLevels(t.game_levels, t.width, t.height);
  if (have == 0 || have == want) return false;
  out = t;
  out.data.clear();
  for (uint32_t f = 0; f < faces; ++f) detail::Chain(t.format, t.width, t.height, t.data.data() + size_t(f) * bytes, have, want, out.data);
  out.mips = want;
  return true;
}

}  // namespace rdr_scene::game_levels
