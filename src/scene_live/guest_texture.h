// One shared reader for "a guest texture handle / object -> format, size, base address, bytes" (Session 103, lane D6; the sign pictures of docs\renderer\CROWD_PROPS_GATES.md 5 and lane F4's particle namer).
// Pure: no game or SDK includes. The game build supplies the memory (`M`: u32(addr) host value of the big-endian word, `fail`) and the physical-memory reader (`phys`).
//
// The chain [C: sub_82294F20 listing 0x82294F20-0x82294F8C; lane F4 docs\formats\particle_items.md, E_inline_callbacks.md 3; H_textures.md 1.1 / 1.2; the adapter's RegisterTextureObject / ShapeOf / GuestLevelLayout /
// TiledOffset2D / SwapBlock / DecodeTexture (src\renderer_adapter\reader.h), whose arithmetic this copies for the 2D block-compressed case]:
//   handle H (the key's word0)  ->  registry R = [0x8392DD9C], entries of 8 bytes {id, wrapper pointer} from [R+8] to [R+16], index = H & 0xFFFF, the stored id must equal H
//   ->  wrapper (96 bytes, vtable 0x8206ADB8): +52 kind (0 = 2D), +80 the library texture header pointer (kind 0)
//   ->  header + 28 .. + 48: the six GPU fetch dwords (tiled bit 31 of dword 0, pitch / 32 in bits 22-30; dword 1: format 0-5, endian 6-7, base address bits 12-31; dword 2: width - 1 in bits 0-12, height - 1 in 13-25;
//       dword 5 bits 9-10: dimension, 1 = 2D)  ->  pixel memory at the PHYSICAL address (dword 1 & 0xFFFFF000).
// [U] The physical -> reader translation is the caller's (the game: REX_KERNEL_MEMORY()->TranslatePhysical); the unit forges it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "texture_registry.h"   // lane F4: the ONE registry walk

namespace rdr_scene::live::guest_tex {

// The registry walk is lane F4's (texture_registry.h WalkTextureHandle: handle -> entry -> wrapper (vtable-checked, kind 0) -> header -> six fetch dwords); this file adds only the memory adapter,
// the shape of the fetch dwords and the pixel read.
// Test only: 1 = the entry's cached pointer is used as the wrapper without the id check (the fault F4's contract forbids); 2 = the tiled bit is ignored; 3 = the endian swap is skipped; 6 = the 4-byte fetch swizzle is not checked (A3).
// (g_tex_plant itself lives in texture_registry.h, so the walk can see plant 7.)

enum class Why : uint8_t { Ok = 0, NoRegistry, NoHandle, WrapperUnreadable, NotPlain2D, NoHeader, Format, Size, Memory };
inline const char* WhyText(Why w) {
  static const char* t[] = {"ok", "no registry", "no handle", "wrapper unreadable", "not a plain 2D texture", "no header", "format not supported", "size not supported", "pixel memory unreadable"};
  return t[uint32_t(w)];
}

struct Resolved {
  uint32_t handle = 0, wrapper = 0, header = 0;
  uint32_t fetch[6] = {};
};

// A crowd-reader memory (`u32`, mutable `fail`) as F4's GuestMem. An unreadable word sets `fail` (the snapshot is then unreadable, as every other read of the reader).
template <class M>
struct MemAdapter final : particles::GuestMem {
  explicit MemAdapter(const M& m) : m_(m) {}
  bool Read32(uint32_t a, uint32_t& out) const override {
    const bool saved = m_.fail;   // review 5: a bad registry / wrapper / header read is THIS picture's why, never the whole snapshot's \fail (inputs_glue.h: the rest of the crowd stays valid)
    m_.fail = false;
    out = m_.u32(a);
    const bool bad = m_.fail;
    m_.fail = (g_tex_plant == 5) ? (saved || bad) : saved;   // (plant 5: the old behaviour, the whole snapshot fails)
    return !bad;
  }
  const M& m_;
};

// handle -> wrapper -> header -> fetch dwords. `cached_ptr` is informational (the plant only).
template <class M>
inline Why ResolveHandle(const M& mem, uint32_t handle, Resolved& out, uint32_t cached_ptr = 0) {
  out = Resolved{};
  out.handle = handle;
  const MemAdapter<M> gm(mem);
  particles::TextureObject o;
  if (g_tex_plant == 1) {   // the fault: trust the cached pointer
    o.wrapper = cached_ptr;
    o.header = mem.u32(cached_ptr + particles::kWrapperTexture);
    for (uint32_t i = 0; i < 6; ++i) o.fetch[i] = mem.u32(o.header + particles::kHeaderFetch + 4 * i);
    particles::FetchToGpuForm(o.fetch);
  } else {
    switch (particles::WalkTextureHandle(gm, handle, o)) {
      case particles::TexWalk::Ok: break;
      case particles::TexWalk::NoHandle: return Why::NoHandle;
      case particles::TexWalk::NotWrapper2D: return Why::NotPlain2D;
      case particles::TexWalk::NoHeader: return Why::NoHeader;
      default: return Why::WrapperUnreadable;
    }
  }
  out.wrapper = o.wrapper, out.header = o.header;
  std::memcpy(out.fetch, o.fetch, sizeof out.fetch);
  return Why::Ok;
}
// The decoded fetch dwords of a plain 2D block-compressed texture (BC1 = game format 18, BC2 = 19, BC3 = 20; S103 PN2: and k_8_8_8_8 = 6 on request).
inline constexpr uint32_t kBgraIdentitySwizzle = 2u | (1u << 3) | (0u << 6) | (3u << 9);   // selectors X, Y, Z, W = memory bytes 2, 1, 0, 3
struct Shape {
  uint32_t width = 0, height = 0, game_format = 0, bw = 4, bh = 4, bpb = 8, base = 0, pitch_blocks = 0, endian = 0;
  uint32_t fetch_swizzle = 0;   // fetch dword 3 bits 1..12: four 3-bit selectors X, Y, Z, W (0..3 = a memory byte of the texel after the endian swap, 4 = zero, 5 = one; reader.h ComposeSwizzle)
  bool tiled = false;
  uint32_t BlocksX() const { return (width + bw - 1) / bw; }
  uint32_t BlocksY() const { return (height + bh - 1) / bh; }
};
// S103 PN2: `allow_8888` also takes game format 6 (k_8_8_8_8: 1x1 blocks of 4 bytes; the UI's A8R8G8B8 pictures, which the file index holds as BGRA8 / RGBA8). Default false: the I2 picture table (quads2d_pictures.h)
// keeps refusing every uncompressed texture, which is also its guard against a surface the GPU draws into; picture_source.h refuses those by CREATOR (the birth hook), not by format.
inline Why ShapeOfFetch(const uint32_t f[6], Shape& s, bool allow_8888 = false) {
  s = Shape{};
  const uint32_t dim = (f[5] >> 9) & 3;
  if (dim != 1) return Why::NotPlain2D;
  s.game_format = f[1] & 0x3Fu;
  if (s.game_format == 18) s.bpb = 8;
  else if (s.game_format == 19 || s.game_format == 20) s.bpb = 16;
  else if (s.game_format == 6 && allow_8888) s.bw = 1, s.bh = 1, s.bpb = 4;
  else return Why::Format;
  s.fetch_swizzle = (f[3] >> 1) & 0xFFFu;
  // A3: a 4-byte picture is handed to the renderer as BGRA8 with an identity view (memory bytes 0, 1, 2, 3 = B, G, R, A), so it is the TRUE picture only when the game's fetch reads X = byte 2, Y = byte 1,
  // Z = byte 0, W = byte 3 (the BGRA identity: selectors 2, 1, 0, 3). Any other order (an A8B8G8R8 identity 0, 1, 2, 3; an X8R8G8B8 whose alpha is forced ONE = 5) is refused by name `format`, never drawn R / B swapped.
  if (s.game_format == 6 && s.fetch_swizzle != kBgraIdentitySwizzle && g_tex_plant != 6) return Why::Format;
  s.tiled = (f[0] >> 31) != 0;
  if (g_tex_plant == 2) s.tiled = false;
  const uint32_t lw = (f[2] & 0x1FFFu) + 1u, lh = ((f[2] >> 13) & 0x1FFFu) + 1u;
  uint32_t log2min = 0;   // review 3: with the packed-mips bit set and min(w, h) <= 16 texels, mip 0 sits in the packed tail (reader.h PackedMipOffset): not read here, never a wrong picture
  while ((1u << log2min) < std::min(lw, lh)) ++log2min;
  if (g_tex_plant != 4 && ((f[5] >> 11) & 1u) && log2min <= 4) return Why::Size;
  const bool border = (f[3] >> 31) != 0;
  const uint32_t adj = border ? 3u : 1u;
  s.width = (f[2] & 0x1FFFu) + adj, s.height = ((f[2] >> 13) & 0x1FFFu) + adj;
  if (!s.width || !s.height || s.width > 4096 || s.height > 4096 || ((s.width & 3) && s.bw == 4) || ((s.height & 3) && s.bh == 4)) return Why::Size;
  s.base = f[1] & 0xFFFFF000u;
  s.endian = (f[1] >> 6) & 3u;
  if (!s.base) return Why::Memory;
  const uint32_t pitch_div32 = (f[0] >> 22) & 0x1FFu;
  const uint32_t row_texels = pitch_div32 << 5;
  const uint32_t bx = (row_texels + s.bw - 1) / s.bw;
  s.pitch_blocks = (bx + 31u) & ~31u;   // GuestLevelLayout (base level): Align(Align(row_texels, bw) / bw, 32)
  if (s.pitch_blocks < s.BlocksX()) return Why::Size;
  return Why::Ok;
}

// A8: a physical read [addr, addr + n) must lie inside the 512 MB of physical memory (the address wraps by the 0x1FFFFFFF mask the way the game's own mapping does): checked by the READER, never left to a
// fault handler (tools\scene\phys_bound_lint.py: every TranslatePhysical in src\scene_live calls this first).
inline bool PhysRangeOk(uint32_t addr, uint32_t n) { return n != 0 && uint64_t(addr & 0x1FFFFFFFu) + n <= 0x20000000ull; }

// The adapter's TiledOffset2D (reader.h) for the 2D tiled layout: x, y in texels of the format's block (here: blocks), pitch in blocks.
inline uint32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bpb) {
  pitch = (pitch + 31u) & ~31u;
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bpb + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return uint32_t(((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (offset & 0x3F));
}

// The adapter's SwapBlock (reader.h).
inline void SwapBlock(uint8_t* p, uint32_t bytes, uint32_t endian) {
  if (endian == 0) return;
  if (g_tex_plant == 3) return;
  if (endian == 3 && bytes >= 4) {
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
      uint32_t v;
      std::memcpy(&v, p + i, 4);
      v = (v << 16) | (v >> 16);
      std::memcpy(p + i, &v, 4);
    }
    return;
  }
  if (endian == 2 && bytes >= 4) {
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
      uint32_t v;
      std::memcpy(&v, p + i, 4);
      v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
      std::memcpy(p + i, &v, 4);
    }
    return;
  }
  if (endian == 1 || (endian == 2 && bytes == 2)) {
    for (uint32_t i = 0; i + 2 <= bytes; i += 2) {
      uint8_t t = p[i];
      p[i] = p[i + 1], p[i + 1] = t;
    }
  }
}

// Mip 0 as linear, little-endian blocks, row-major (BlocksX x BlocksY x bpb bytes): what the renderer's BC image takes. `phys(addr, n, dst)` reads n bytes of pixel memory at the physical address.
inline constexpr uint32_t kMaxRead = 1u << 20;   // review 4: nothing past 1 MB is read by DEFAULT (an oversize texture is Why::Size)
inline constexpr uint32_t kMaxReadHard = 8u << 20;   // S103 PN2: a caller may ask for up to 8 MB (a 512 x 1024 A8R8G8B8 menu picture is 2 MB); nothing past this, whatever the caller asks
template <class P>
inline Why ReadMip0(const Shape& s, P&& phys, std::vector<uint8_t>& out, uint32_t max_bytes = kMaxRead) {
  const uint32_t bx = s.BlocksX(), by = s.BlocksY(), log2_bpb = s.bpb == 8 ? 3u : (s.bpb == 4 ? 2u : 4u);
  const uint32_t lim = std::min(max_bytes, kMaxReadHard);
  if (uint64_t(bx) * by * s.bpb > lim) return Why::Size;   // checked BEFORE any read
  out.assign(size_t(bx) * by * s.bpb, 0);
  if (!s.tiled) {
    const uint32_t row_bytes = s.pitch_blocks * s.bpb;
    const uint64_t ext = uint64_t(row_bytes) * by;
    if (ext > (g_tex_plant == 8 ? lim : kMaxReadHard) || (g_tex_plant != 10 && ext > uint64_t(lim) + 32ull * s.bpb * by)) return Why::Size;   // S103 CSF review A: padding is at most 32 blocks a row; a bad pitch is refused before the 8 MB allocation / read   // S103 CSF: the pitch-PADDED source extent (360 habit H-CS2: rows padded to 32 blocks) is bounded by the hard cap; the payload was bounded above by lim (same class as TX on the tiled branch)
    std::vector<uint8_t> src(size_t(row_bytes) * by);
    if (!phys(s.base, uint32_t(src.size()), src.data())) return Why::Memory;
    for (uint32_t y = 0; y < by; ++y) std::memcpy(out.data() + size_t(y) * bx * s.bpb, src.data() + size_t(y) * (g_tex_plant == 9 ? bx * s.bpb : row_bytes), size_t(bx) * s.bpb);   // (plant 9 = a wrong stride: the padding is not stepped over)
  } else {
    uint32_t top = 0;
    for (uint32_t y = 0; y < by; ++y)
      for (uint32_t x = 0; x < bx; ++x) top = std::max(top, TiledOffset2D(int32_t(x), int32_t(y), s.pitch_blocks, log2_bpb) + s.bpb);
    const uint64_t rect = uint64_t((bx + 31u) & ~31u) * ((by + 31u) & ~31u) * s.bpb;   // S103 CSF review 5: the padded RECTANGLE (whole 32x32-block tiles) the picture can touch; a wider pitch than the picture needs puts the tile rows further apart and is refused before the allocation / read
    if (top > kMaxReadHard || (g_tex_plant != 10 && top > rect)) return Why::Size;
    // S103 TX: the padded TILE extent is bounded by the hard read cap; the caller's max_bytes bounds the PICTURE payload (checked above). PN2 (16699681f) had made `top > lim` too, so a 64x64 BC1 sign (2048 B payload, ~4 KB+ tiled extent) failed Why::Size under the crowd's 4096 cap
    std::vector<uint8_t> src(top);
    if (!phys(s.base, top, src.data())) return Why::Memory;
    for (uint32_t y = 0; y < by; ++y)
      for (uint32_t x = 0; x < bx; ++x)
        std::memcpy(out.data() + (size_t(y) * bx + x) * s.bpb, src.data() + TiledOffset2D(int32_t(x), int32_t(y), s.pitch_blocks, log2_bpb), s.bpb);
  }
  for (size_t i = 0; i < out.size(); i += s.bpb) SwapBlock(out.data() + i, s.bpb, s.endian);
  return Why::Ok;
}

// handle -> shape + bytes in one call (the fetch dwords are returned too: the caller keys its cache on handle + wrapper + fetch[1]).
template <class M, class P>
inline Why ReadHandleTexture(const M& mem, uint32_t handle, P&& phys, Resolved& r, Shape& s, std::vector<uint8_t>& blocks, uint32_t max_bytes = kMaxRead, bool allow_8888 = false) {
  if (const Why w = ResolveHandle(mem, handle, r); w != Why::Ok) return w;
  if (const Why w = ShapeOfFetch(r.fetch, s, allow_8888); w != Why::Ok) return w;
  return ReadMip0(s, phys, blocks, max_bytes);
}

}  // namespace rdr_scene::live::guest_tex
