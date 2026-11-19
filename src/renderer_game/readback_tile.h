// S103 lane RBC: the guest-memory writer for the stream-free build's GPU read-backs (docs\research\s103\RBK.md 4).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure, header-only: std + guest_view.h only (no game, REX or renderer includes);
// tools\readback\rbc_unit.cpp builds THIS file.
//
// The game renders a picture, resolves it into a texture and reads the texture's bits with the CPU (the lock). In the stream-free build nothing
// executes the resolve, so at the lock we write OUR picture into the texture's memory exactly where, and the way, the 360's resolve would have
// left it, and the game's own CPU code (untile / row copy) reads it unchanged.
//
// It is a NEW port, not a move: the stream build's WriteCopyBack (src\renderer_adapter\reader.h) is untouched and still serves the stream build.
// What it takes over from there (reader.h ShapeOf :1309, GuestLevelLayout :2876 base case, TiledOffset2D :2910, SwapBlock :2918):
//   the six fetch dwords at texture + 28 .. + 48 (host values of the header words, CPU-view addresses) give
//     dword 0: bit 31 tiled, bits 22-30 pitch / 32            dword 1: bits 0-5 format (6 = k_8_8_8_8, 2 = k_8), bits 6-7 endian, bits 12-31 base address
//     dword 2: width - 1 (bits 0-12), height - 1 (bits 13-25)  dword 3: bit 31 border, bits 1-12 the four selectors X Y Z W of the fetch swizzle
//     dword 5: bits 9-10 dimension (1 = 2D)
//   the base address is the CPU view address's physical form (guest_view::ToPhysical).
// SWAP (the channel order) comes from the fetch swizzle, not from a resolve record: the game's fetch reads channel k = byte sel_k of the texel (after the
// endian swap) for sel_k < 4, so our channel k is stored in byte sel_k. Identity (0 1 2 3) = our R G B A in memory order; the BGRA identity (2 1 0 3)
// = R and B exchanged. A selector of 4 / 5 (zero / one) is not stored; a byte no channel names is written 255.
// Level 0 of a 2D texture only; formats 6 and 2 only (anything else is refused with a reason, never written wrong).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../guest_view.h"

namespace wwe2k14::readback_tile {

#ifdef RBC_PLANTS
// Test only (tools\readback\rbc_unit.cpp, built by tools\readback\rbc_check.py): 1 = the swizzle is ignored (identity order always: the "wrong swap");
// 2 = the endian swap is skipped; 3 = the tiled bit is ignored (linear layout); 4 = the box filter is nearest at the top-left corner of each box;
// 5 = the site guard (size, packed mips, writable pages) is skipped; 6 = a site with no scene picture writes black anyway (readback_ours.h ServeSite).
inline int g_plant = 0;
#define RBC_PLANT(n) (::wwe2k14::readback_tile::g_plant == (n))
#else
#define RBC_PLANT(n) false
#endif

enum class Why : uint8_t { Ok = 0, Not2D, Format, Size, NoAddress, NoPicture, Level, SizeMismatch, NotWritable, Packed, NoScene, TimedOut, Rect, SwitchedOff };
inline const char* WhyText(Why w) {
  static const char* const t[] = {"ok", "not a plain 2D texture", "format not supported", "size not supported", "no address", "no picture", "level not 0", "not the size this site writes", "pages not committed / writable", "packed mip tail", "no 3D scene to copy", "no picture within the timeout", "crop rect unusable", "switched off"};
  return t[uint32_t(w)];
}

struct Plan {
  uint32_t width = 0, height = 0, format = 0, bpb = 4, endian = 0, base = 0, pitch_blocks = 0, row_pitch_bytes = 0;
  uint32_t sel[4] = {0, 1, 2, 3};   // the fetch swizzle: for our channel k, the memory byte it is stored in (>= 4: not stored)
  bool tiled = false, packed = false;   // packed: dword 5 bit 11 (the small mips live in a packed tail; with min(w, h) <= 16 level 0 is IN the tail, which this writer does not lay out)
  // The bytes the level spans from `base` (tiled: whole 32 x 32 tiles).
  uint64_t ExtentBytes() const {
    const uint64_t rows = tiled ? ((uint64_t(height) + 31) & ~31ull) : height;
    return rows * row_pitch_bytes;
  }
};

constexpr uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
constexpr uint32_t Log2Floor(uint32_t v) {
  uint32_t r = 0;
  while (v > 1) v >>= 1, ++r;
  return r;
}

// The Xenos 2D tiling (reader.h TiledOffset2D): the byte offset of texel (x, y) for a pitch in blocks and log2 of the block size.
inline uint32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bpb) {
  pitch = Align(pitch, 32);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bpb + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return uint32_t(((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (offset & 0x3F));
}

// The endian swap of one texel (the 32-bit forms; each is its own inverse).
inline void SwapTexel(uint8_t* p, uint32_t bytes, uint32_t endian) {
  if (endian == 0 || RBC_PLANT(2)) return;
  if (endian == 3 && bytes >= 4) {   // the two 16-bit halves exchanged (reader.h SwapBlock endian 3)
    uint32_t v;
    std::memcpy(&v, p, 4);
    v = (v << 16) | (v >> 16);
    std::memcpy(p, &v, 4);
  } else if (endian == 2 && bytes >= 4) {   // 32-bit byte swap (endian 2)
    uint32_t v;
    std::memcpy(&v, p, 4);
    v = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    std::memcpy(p, &v, 4);
  } else if (endian == 1 && bytes >= 2) {   // bytes swapped inside each 16-bit half (endian 1)
    for (uint32_t i = 0; i + 2 <= bytes; i += 2) std::swap(p[i], p[i + 1]);
  }
}

// The six fetch dwords -> a Plan. `w` are the header words as host values (CPU-view addresses in dword 1).
inline Why MakePlan(const uint32_t w[6], Plan& p) {
  p = Plan{};
  if (((w[5] >> 9) & 3) != 1) return Why::Not2D;
  p.format = w[1] & 0x3F;
  if (p.format == 6) p.bpb = 4;
  else if (p.format == 2) p.bpb = 1;
  else return Why::Format;
  const bool border = (w[3] >> 31) != 0;
  const uint32_t adj = border ? 3u : 1u;
  p.width = (w[2] & 0x1FFFu) + adj;
  p.height = ((w[2] >> 13) & 0x1FFFu) + adj;
  if (!p.width || !p.height || p.width > 8192 || p.height > 8192) return Why::Size;
  p.tiled = (w[0] >> 31) != 0 && !RBC_PLANT(3);
  p.endian = (w[1] >> 6) & 3u;
  p.packed = ((w[5] >> 11) & 1) != 0;
  p.base = wwe2k14::guest_view::ToPhysical(w[1]) & 0xFFFFF000u;
  if (!p.base) return Why::NoAddress;
  const uint32_t pitch_div32 = (w[0] >> 22) & 0x1FFu;
  p.pitch_blocks = Align(pitch_div32 << 5, 32);   // GuestLevelLayout base case: Align(Align(row_texels, bw) / bw, 32), bw = 1
  if (p.pitch_blocks < p.width) return Why::Size;
  p.row_pitch_bytes = p.pitch_blocks * p.bpb;
  const uint32_t s = w[3] >> 1;
  for (uint32_t k = 0; k < 4; ++k) p.sel[k] = RBC_PLANT(1) ? k : (s >> (3 * k)) & 7;
  return Why::Ok;
}

inline uint32_t OffsetOf(const Plan& p, uint32_t x, uint32_t y) {
  return p.tiled ? TiledOffset2D(int32_t(x), int32_t(y), p.pitch_blocks, Log2Floor(p.bpb)) : y * p.row_pitch_bytes + x * p.bpb;
}

// The texel's memory bytes for our RGBA channels `c`.
inline void EncodeTexel(const Plan& p, const uint8_t c[4], uint8_t* out) {
  if (p.format == 2) {
    out[0] = c[0];
    return;
  }
  out[0] = out[1] = out[2] = out[3] = 255;
  for (uint32_t k = 0; k < 4; ++k)
    if (p.sel[k] < 4) out[p.sel[k]] = c[k];
  SwapTexel(out, 4, p.endian);
}

// The inverse (tests): the memory bytes of a texel -> our RGBA channels (channels no selector reaches read 255).
inline void DecodeTexel(const Plan& p, const uint8_t* in, uint8_t c[4]) {
  if (p.format == 2) {
    c[0] = in[0], c[1] = c[2] = 0, c[3] = 255;
    return;
  }
  uint8_t b[4];
  std::memcpy(b, in, 4);
  SwapTexel(b, 4, p.endian);
  for (uint32_t k = 0; k < 4; ++k) c[k] = p.sel[k] < 4 ? b[p.sel[k]] : 255;
}

// Writes `rgba` (rw x rh, our channel order) over the whole level, nearest-scaled to the level's size. `mem` is the level's first byte (the base address's host pointer).
inline Why WriteLevel0(const Plan& p, uint8_t* mem, const uint8_t* rgba, uint32_t rw, uint32_t rh) {
  if (!mem) return Why::NoAddress;
  if (!rgba || !rw || !rh) return Why::NoPicture;
  for (uint32_t y = 0; y < p.height; ++y) {
    const uint32_t sy = std::min(rh - 1, uint32_t(uint64_t(y) * rh / p.height));
    for (uint32_t x = 0; x < p.width; ++x) {
      const uint32_t sx = std::min(rw - 1, uint32_t(uint64_t(x) * rw / p.width));
      uint8_t out[4];
      EncodeTexel(p, rgba + (size_t(sy) * rw + sx) * 4, out);
      std::memcpy(mem + OffsetOf(p, x, y), out, p.bpb);
    }
  }
  return Why::Ok;
}

// Tests: the level read back as RGBA (width x height).
inline void ReadLevel0(const Plan& p, const uint8_t* mem, std::vector<uint8_t>& rgba) {
  rgba.assign(size_t(p.width) * p.height * 4, 0);
  for (uint32_t y = 0; y < p.height; ++y)
    for (uint32_t x = 0; x < p.width; ++x) DecodeTexel(p, mem + OffsetOf(p, x, y), &rgba[(size_t(y) * p.width + x) * 4]);
}

// A box-filter downscale (each destination texel = the mean of the source texels it covers) to `dw` x `dh`; an enlargement is nearest. Used to bring the presented picture to a small
// destination (the 128 x 128 clip thumbnail) without the aliasing a nearest pick of every 10th texel gives.
inline void ScaleBox(const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, std::vector<uint8_t>& out) {
  out.assign(size_t(dw) * dh * 4, 0);
  if (!src || !sw || !sh || !dw || !dh) return;
  for (uint32_t y = 0; y < dh; ++y) {
    const uint32_t y0 = uint32_t(uint64_t(y) * sh / dh);
    const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * sh / dh));
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = uint32_t(uint64_t(x) * sw / dw);
      const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * sw / dw));
      uint64_t sum[4] = {};
      uint32_t n = 0;
      const uint32_t ye = RBC_PLANT(4) ? y0 + 1 : std::min(y1, sh), xe = RBC_PLANT(4) ? x0 + 1 : std::min(x1, sw);
      for (uint32_t yy = y0; yy < ye; ++yy)
        for (uint32_t xx = x0; xx < xe; ++xx) {
          const uint8_t* s = src + (size_t(yy) * sw + xx) * 4;
          sum[0] += s[0], sum[1] += s[1], sum[2] += s[2], sum[3] += s[3];
          ++n;
        }
      uint8_t* d = &out[(size_t(y) * dw + x) * 4];
      for (int k = 0; k < 4; ++k) d[k] = n ? uint8_t((sum[k] + n / 2) / n) : 0;
    }
  }
}

}  // namespace wwe2k14::readback_tile
