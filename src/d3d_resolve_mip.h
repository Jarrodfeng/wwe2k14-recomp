// wwe2k14 - the library's mip-level / face addressing for a resolve's destination, transcribed from the listings (Session 87,
// F2): 0x82CA78F8 (313 instructions) with the helpers 0x82CAC9B0 (texture class), 0x82CA6AA8 (size unpack), 0x82CA6D30
// (dimension alignment, uses the block dimensions 0x82CA6C10 and the bits-per-element table at 0x820823B0) and 0x82CA71D0
// (packed-mip tail offset). The resolve core (0x82CAEA30, call at 0x82CAEBE4 when the level or the slice argument is non-zero)
// uses four of its outputs: the pitch in bytes ([156]), the destination base ([140]), the mip height ([128]) and width
// ([132]). Everything is PowerPC integer arithmetic on the texture header words; each step below is one or two instructions and
// names the register of the listing so it can be checked line by line. Guest memory is read through the exe's base pointer.
//
// Not covered (the caller falls back to the original): a 3D destination (dimension 2: the resolve core's r22 = 8 and the slice
// bits of the destination info), and the listing's one unreachable exit (0x82CA7D44, a negative level counter).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace wwe2k14::d3d_resolve_mip {

inline uint32_t G(const uint8_t* m, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, m + a + (a >= 0xE0000000u ? 0x1000u : 0u), 4);
  return std::byteswap(be);
}
inline uint32_t Mask(int mb, int me) { return (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)); }
inline uint32_t Rlwinm(uint32_t v, int sh, int mb, int me) { return std::rotl(v, sh) & Mask(mb, me); }
// slw / srw: the shift amount is the low 6 bits; 32 and above give 0.
inline uint32_t Slw(uint32_t v, uint32_t n) { n &= 0x3F; return n >= 32 ? 0 : v << n; }
inline uint32_t Srw(uint32_t v, uint32_t n) { n &= 0x3F; return n >= 32 ? 0 : v >> n; }
inline uint32_t Clz(uint32_t v) { return uint32_t(std::countl_zero(v)); }

// 0x82CA6C10: block width and height of a texture format.
inline void BlockDims(uint32_t fmt, uint32_t& w, uint32_t& h) {
  w = h = 1;
  if (fmt > 44) {
    if (fmt == 49) w = h = 4;
    else if (fmt <= 50) w = h = 1;
    else if (fmt <= 53) w = h = 4;
    else if (fmt <= 57) w = h = 1;
    else if (fmt <= 61) w = h = 4;
    return;
  }
  if (fmt == 44) { w = 4; h = 1; return; }
  if (fmt < 11) return;
  if (fmt <= 12) { w = 2; h = 1; return; }
  if (fmt <= 17) return;
  if (fmt <= 20) { w = h = 4; return; }
  if (fmt == 39) { w = 4; h = 1; return; }
  if (fmt == 40) { w = 2; h = 1; }
}

// 0x82CAC9B0: the texture class (3 = plain 2D / cube, 17-20 the packed-mip classes, 16 a surface view, else the type nibble).
inline uint32_t TextureClass(const uint8_t* m, uint32_t tex) {
  const uint32_t w0 = G(m, tex + 0);
  uint32_t cls = w0 & 0xF;
  if (cls == 3) {
    const uint32_t dim = Rlwinm(G(m, tex + 48), 23, 30, 31);
    if (dim == 3) cls = 18;
    else if (dim == 2) cls = 17;
    else if (dim == 0) cls = 20;
    else if (G(m, tex + 32) & 0x400) cls = 19;
  } else if (cls == 4) {
    if (w0 & 0x40000000u) {
      const uint32_t other = G(m, tex + 24);
      if ((G(m, other + 48) & 0x600) == 1024) cls = 16;
    }
  }
  return cls;
}

// 0x82CA6AA8: the texture's width, height and depth in texels (the size dword at +36, the +1 / +3 adjustment of dword +40).
inline void TextureSize(const uint8_t* m, uint32_t tex, uint32_t& w, uint32_t& h, uint32_t& d) {
  const uint32_t w36 = G(m, tex + 36);
  const uint32_t adj = Rlwinm(G(m, tex + 40), 2, 30, 30) + 1;
  const uint32_t dim = Rlwinm(G(m, tex + 48), 23, 30, 31);
  if (dim == 0) {
    w = (w36 & 0x00FFFFFFu) + adj;
    h = 1;
    d = 1;
  } else if (dim == 2) {
    w = (w36 & 0x7FF) + adj;
    h = Rlwinm(w36, 21, 21, 31) + adj;
    d = Rlwinm(w36, 10, 22, 31) + adj;
  } else {
    w = (w36 & 0x1FFF) + adj;
    h = Rlwinm(w36, 19, 19, 31) + adj;
    d = 1;
  }
}

// 0x82CA6D30: rounds the dimensions up to the tile granularity; returns the size in bytes (4 KB aligned).
inline uint32_t AlignDims(const uint8_t* m, uint32_t& w, uint32_t& h, uint32_t& d, uint32_t bpe, uint32_t fmt,
                          uint32_t dim, uint32_t flag) {
  (void)m;
  uint32_t r25 = 32;
  const uint32_t r24 = dim == 0 ? 1u : 32u;      // subfic / subfe / clrlwi 27, + 1
  const uint32_t r27 = dim == 2 ? 4u : 1u;
  uint32_t bw, bh;
  BlockDims(fmt, bw, bh);
  if (flag == 0) {
    uint32_t r10 = (bw * bh * bpe) >> 3;
    if (r10) {
      r10 = 256u / r10;
      if (r10 >= 32) r25 = r10;
    }
  }
  const uint32_t unit_w = bw * r25;
  w = (w + unit_w - 1) & ~(unit_w - 1);          // r10 = (*r31 + r11 - 1) andc (r11 - 1)
  const uint32_t unit_h = bh * r24;
  h = (h + unit_h - 1) & ~(unit_h - 1);
  d = (d + r27 - 1) & ~(r27 - 1);
  const uint32_t row = ((w * bpe) >> 3) * h;
  if (dim != 2) return ((row + 4095) & ~4095u) * d;
  return ((d * row) + 4095) & ~4095u;
}

// 0x82CA71D0: the offset of a packed-mip tail level (level, its padded width / height / depth in texels, the pitch in bytes
// and the slice size in bytes of the level, the texture format).
inline uint32_t PackedTail(const uint8_t* m, uint32_t level, uint32_t w, uint32_t h, uint32_t d, uint32_t pitch_bytes,
                           uint32_t slice_bytes, uint32_t fmt) {
  const uint32_t bpe = m[0x820823B0u + 1 + 2 * fmt];
  uint32_t s[8] = {};  // the listing's stack slots +80 .. +108 as s[(offset - 80) / 4]
  uint32_t bw, bh;
  BlockDims(fmt, bw, bh);
  s[0] = bw;  // [80]
  s[1] = bh;  // [84]
  const uint32_t r11 = 32 - Clz(w - 1), r10 = 32 - Clz(h - 1), r8 = 32 - Clz(d - 1);
  const bool carry = r10 >= r11;                // subfc r9,r11,r10: carry = no borrow
  const uint32_t sel = carry ? 0u : 1u;         // subfe r7,r7,r7 ; clrlwi 31
  s[2] = Slw(1, r11);                           // [88]
  s[3] = Slw(1, r10);                           // [92]
  uint32_t tail;                                // r11 of the listing at 0x82CA7294
  if (level < 3) {
    s[4 + sel] = uint32_t(16) >> level;         // [96 + 4 sel]
    tail = s[6];                                // [104] (0)
  } else {
    const uint32_t other = sel ^ 1u;
    s[4 + other] = Srw(s[2 + other], level - 2);
    if (s[4 + other] >= 4) {
      tail = s[6];
    } else {
      tail = r8 - level;
      if (tail <= 1) tail = 1;
      tail <<= 2;
    }
  }
  const uint32_t r7 = (s[0] * bpe * s[4]) >> 3;                   // ((bw * bpe) * [96]) >> 3
  const uint32_t r6 = r7;
  const uint32_t r7b = s[5] * pitch_bytes;                        // [100] * pitch
  return (r6 + r7b) + tail * slice_bytes;
}

struct MipOut {
  uint32_t pitch_bytes = 0;  // [156]
  uint32_t base = 0;         // [140]
  uint32_t height = 0;       // [128]
  uint32_t width = 0;        // [132]
};

// 0x82CA78F8 as the resolve core calls it (0x82CAEBE4): tex = the destination texture header, slice = the slice argument
// rounded as the core does (a 2D texture: unchanged), level = the level argument. False: not described.
inline bool Compute(const uint8_t* m, uint32_t tex, uint32_t slice, uint32_t level, MipOut& out) {
  const uint32_t w28 = G(m, tex + 28), w32 = G(m, tex + 32), w40 = G(m, tex + 40), w48 = G(m, tex + 48);
  const uint32_t fmt = w32 & 0x3F;
  const uint32_t bpe = m[0x820823B0u + 1 + 2 * fmt];          // r14
  uint32_t r30 = Rlwinm(w40, 1, 31, 31);
  const uint32_t r28_bit = Rlwinm(w28, 1, 31, 31);
  const uint32_t r24 = Rlwinm(w48, 23, 30, 31);               // dimension
  const uint32_t r29 = r30 * 2;
  if (r24 == 2) return false;                                  // 3D: not described
  uint32_t s116, s120, s108;                                   // width, height, depth of the base level
  TextureSize(m, tex, s116, s120, s108);
  const uint32_t r7 = s116, r23 = s120;
  const uint32_t r9 = r7 - r29, r8 = r23 - r29;
  const uint32_t sh_w = r30 - Clz(r9 - 1) + 32, sh_h = r30 - Clz(r8 - 1) + 32;
  uint32_t r26 = Slw(1, sh_w);
  uint32_t r17 = Slw(1, sh_h);
  uint32_t r15 = 1;
  if (r24 == 2) r15 = Slw(1, r30 - Clz(s108 - r29 - 1) + 32);
  // The packed-mip flag: small enough and the header's bit 0x800 (0x82CA79E8-0x82CA7A0C).
  bool packed;
  if (r26 <= 16 || r17 <= 16) packed = (w48 & 0x800) != 0;
  else packed = false;
  r30 = packed ? 1u : 0u;
  uint32_t s104 = ((level != 0 && !packed) ? w48 : w32) & 0xFFFFF000u;  // the base the offsets add to
  uint32_t out_r30 = 0, out_r17 = 0, out_r15 = 0;
  uint32_t s124 = 0;
  if (level == 0 && !packed) {
    // 0x82CA7A40: the base level, not packed.
    uint32_t aw = r7, ah = r23, ad = s108;
    AlignDims(m, aw, ah, ad, bpe, fmt, r24, r28_bit);
    const uint32_t w44 = G(m, tex + 44);
    const bool plain = (w28 & 0x80000000u) || (w32 & 0x400) || (w48 & 0x800) || r24 != 1 || (w44 & 0x3C0) ||
                       (w40 & 0x80000000u);
    uint32_t v17;
    if (plain) {
      v17 = ah;                                               // [100]: the aligned height
    } else {
      uint32_t bw, bh;
      BlockDims(fmt, bw, bh);
      v17 = (bh + r23 - 1) & ~(bh - 1);                       // andc r17,(bh + h - 1),(bh - 1)
    }
    out_r17 = v17;
    out_r15 = ad;                                             // [96]
    if (r24 == 0) {
      out_r30 = (aw * bpe) >> 3;
    } else {
      const uint32_t r11 = Rlwinm(w28, 10, 23, 31) * bpe;
      out_r30 = Rlwinm(r11, 2, 3, 29);
    }
  } else {
    // 0x82CA7B14: a lower level, or a packed texture.
    uint32_t r10 = Srw(r9, level);
    if (r10 <= 1) r10 = 1;
    uint32_t r11 = Srw(r8, level);
    s116 = r10 + r29;
    if (r11 <= 1) r11 = 1;
    s120 = r11 + r29;
    if (r24 == 2) {
      uint32_t d = Srw(s108 - r29, level);
      if (d <= 1) d = 1;
      s108 = d + r29;
    } else {
      s108 = 1;
    }
    uint32_t r16 = 1;
    const uint32_t cls = TextureClass(m, tex);
    if (cls == 18 || cls == 19) {
      const uint32_t a = Rlwinm(w32, 23, 30, 30);
      const uint32_t b = Rlwinm(G(m, tex + 36), 6, 26, 31);
      const uint32_t one = Slw(1, a);
      r16 = (b + one) & ~(one - 1);
    }
    uint32_t r22 = 31 - Clz(r26), r23l = 31 - Clz(r17), r24l = 31 - Clz(r15);
    int32_t r18;
    if (r30) {
      r18 = int32_t(level);
      ++r22, ++r23l, ++r24l;
    } else {
      r18 = int32_t(level) - 1;
    }
    if (r18 < 0) return false;                                 // 0x82CA7D44: unreachable in the listing's flow
    const uint32_t r19 = Rlwinm(w28, 1, 31, 31), r21 = w32 & 0x3F, r20 = w48;
    uint32_t s112 = 0, s100 = 0, s96 = 0;
    uint32_t r30l = 0;
    for (;;) {
      if (r22) --r22;
      if (r23l) --r23l;
      if (r24l) --r24l;
      s112 = Slw(1, r22);
      s100 = Slw(1, r23l);
      s96 = Slw(1, r24l);
      const uint32_t r26l = Rlwinm(r20, 23, 30, 31);
      const uint32_t w_unaligned = s112, h_unaligned = s100, d_unaligned = s96;
      AlignDims(m, s112, s100, s96, bpe, r21, r26l, r19);
      r30l = (s112 * bpe) >> 3;                                // pitch bytes of this mip
      const bool to_tail = (w_unaligned <= 16 || h_unaligned <= 16) && (r20 & 0x800);
      // (0x82CA7C6C-0x82CA7C88: the compare is on the UNALIGNED sizes r29 / r28 stored before the call)
      if (to_tail) {
        out_r17 = s100;                                        // [100] after the call, as 0x82CA7CF0 reloads it
        out_r15 = s96;
        const uint32_t add = PackedTail(m, uint32_t(r18), w_unaligned, h_unaligned, d_unaligned, r30l, r30l * out_r17, r21);
        s104 += add;
        s124 = add;
        break;
      }
      out_r17 = s100;
      out_r15 = s96;
      if (r18 > 0) {
        uint32_t size;
        if (r26l == 2) {
          size = (r30l * out_r15 * out_r17 + 4095) & ~4095u;
        } else {
          size = ((r30l * out_r17 + 4095) & ~4095u) * out_r15;
        }
        s104 += size * r16;
      }
      if (--r18 < 0) break;
    }
    out_r30 = r30l;
  }
  const uint32_t r11 = out_r30 * out_r17;
  const uint32_t aligned = (r11 + 4095) & ~4095u;
  out.base = aligned * slice + s104;
  out.pitch_bytes = out_r30;
  out.height = s120;
  out.width = s116;
  (void)out_r15;
  (void)s124;
  return true;
}

}  // namespace wwe2k14::d3d_resolve_mip
