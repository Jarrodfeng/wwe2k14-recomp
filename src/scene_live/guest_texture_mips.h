// A guest texture's WHOLE mip chain as linear BC blocks (Session 108, lane PRT; docs\research\s108\PRT_plan.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: no game or SDK includes. The game build supplies `phys` (the glue's physical reader, inputs_glue.h CrowdGuestMem::phys: the A8 range check, the page check and a fault-safe copy).
//
// guest_texture.h reads mip 0 (ReadMip0); this adds the stored levels the header names, with the adapter's own arithmetic (src\renderer_adapter\reader.h the level count,
// :2907-2921 GuestLevelLayout for a non-base level, :2923-2939 PackedMipOffset, :2991-3010 the mip offsets, :3040-3089 the level read), for the one case the particle sprites are: 2D, one
// layer, BC1 / BC2 / BC3 (game formats 18 / 19 / 20). Proof: mip 0 [C] (tools\particles\tex_owner.py joined the adapter's captured particle images to the prtcls.pac DDS files, mip 0 only);
// levels >= 1 [L] (the adapter's / Xenia's layout, never compared with these files: that capture held ONE level per particle image, prtcls_pac.md "Sprite textures in the draw stream").
// The live proof is the dev self-check (particle_namer.h CheckAgainstFile: guest_check equal / differ / short; `short` = the header named fewer levels than the file).
//   level count  = min(fetch dword 4 bits 6-9, log2floor(max(w, h))) + 1; 1 when dword 5 has no mip page
//   mip base     = dword 5 & 0xFFFFF000 (already in GPU form: texture_registry.h FetchToGpuForm)
//   packed tail  = dword 5 bit 11: levels >= max(log2ceil(min(w, h)) - 4, 0) live in ONE stored level (the first tail level) at the PackedMipOffset block offset; S108 CAP2: when that level is 0,
//                  mip 0 is read from the BASE's tail (guest_texture.h Shape::tail_x / tail_y) and levels >= 1 from the mips' tail at the mip address (stored level 0, mip strides)
//   level k >= 1 = pitch Align(Align(max(NextPow2(w) >> k, 1), 4) / 4, 32) blocks (untiled: the row bytes aligned to 256), rows Align(blocks, 32), slice Align(row bytes x rows, 4096); the levels follow each other from the mip base.
// Every read goes through `phys` with a bounded size (one level at a time, at most kMaxReadHard); a refused read is Why::Memory, an impossible shape Why::Size: never a guess.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "guest_texture.h"

namespace rdr_scene::live::guest_tex {

// Test only (prt_unit): 11 = the packed-tail block offset is ignored (every tail level read from the tail's corner); 12 = every level uses the base level's pitch.
// (g_tex_plant itself lives in texture_registry.h.)

inline uint32_t MipLog2Ceil(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) ++r; return r; }
inline uint32_t MipLog2Floor(uint32_t v) { uint32_t r = 0; while (v > 1) v >>= 1, ++r; return r; }
inline uint32_t MipNextPow2(uint32_t v) { return 1u << MipLog2Ceil(v); }
inline uint32_t MipAlign(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

struct Chain {
  uint32_t width = 0, height = 0, game_format = 0, mips = 0;
  std::vector<uint8_t> data;   // mip 0 first, each level tightly packed (BlocksX x BlocksY x bpb), little-endian blocks: what assets::Texture holds for a DDS
};

// The level count the game's fetch names (reader.h ShapeOf, L12).
inline uint32_t FetchMipCount(const uint32_t f[6], uint32_t width, uint32_t height) {
  uint32_t max_level = (f[4] >> 6) & 0xFu;
  if (!(f[5] >> 12)) max_level = 0;
  return std::min(max_level, MipLog2Floor(std::max(width, height))) + 1;
}

// The pitch (blocks), row bytes and slice stride of a NON-base stored level (GuestLevelLayout, base = false, 2D).
struct MipLayout { uint32_t pitch_blocks = 0, row_bytes = 0, slice = 0; };
inline MipLayout LevelLayoutOf(const Shape& s, uint32_t level) {
  MipLayout l;
  const uint32_t row_texels = std::max(MipNextPow2(s.width) >> level, 1u), rows_texels = std::max(MipNextPow2(s.height) >> level, 1u);
  l.pitch_blocks = g_tex_plant == 12 ? s.pitch_blocks : MipAlign(MipAlign(row_texels, s.bw) / s.bw, 32);
  l.row_bytes = l.pitch_blocks * s.bpb;
  if (!s.tiled) l.row_bytes = MipAlign(l.row_bytes, 256);
  const uint32_t block_rows = MipAlign(MipAlign(rows_texels, s.bh) / s.bh, 32);
  l.slice = MipAlign(l.row_bytes * block_rows, 4096);
  return l;
}

// The block offset of `mip` inside the packed tail: the SDK's GetPackedMipOffset (guest_texture.h PackedMipOffset, S108 CAP2: the one port, shared with mip 0's tail offset).
inline void TailOffset(uint32_t width, uint32_t height, const Shape& s, uint32_t mip, uint32_t& x, uint32_t& y) {
  PackedMipOffset(width, height, s.bw, s.bh, mip, x, y);
  if (g_tex_plant == 11) x = y = 0;
}

// The whole chain. Mip 0 is ReadMip0's (the same bytes the namer's key hashes). `max_bytes` bounds mip 0 as ReadMip0 does; the chain is at most 4/3 of it.
template <class P>
inline Why ReadChain(const uint32_t f[6], P&& phys, Chain& out, uint32_t max_bytes = kMaxRead) {
  out = Chain{};
  Shape s;
  if (const Why w = ShapeOfFetch(f, s); w != Why::Ok) return w;
  if (s.bw != 4) return Why::Format;   // BC only (ShapeOfFetch without allow_8888 already refuses the rest)
  if (const Why w = ReadMip0(s, phys, out.data, max_bytes); w != Why::Ok) return w;
  out.width = s.width, out.height = s.height, out.game_format = s.game_format;
  const uint32_t levels = FetchMipCount(f, s.width, s.height);
  out.mips = 1;
  const uint32_t mip_addr = f[5] & 0xFFFFF000u;
  if (levels <= 1 || !mip_addr) return Why::Ok;
  const bool packed = ((f[5] >> 11) & 1u) != 0;
  const uint32_t log2min = MipLog2Ceil(std::min(s.width, s.height));
  const uint32_t packed_level = packed ? (log2min > 4 ? log2min - 4 : 0) : 0xFFFFFFFFu;
  const uint32_t max_level = levels - 1, log2_bpb = s.bpb == 8 ? 3u : 4u;
  std::array<uint32_t, 16> offs{};
  uint32_t acc = 0;
  for (uint32_t level = 1; level <= std::min(max_level, packed_level) && level < 16; ++level) offs[level] = acc, acc += LevelLayoutOf(s, level).slice;
  std::vector<uint8_t> src;
  for (uint32_t level = 1; level <= max_level && level < 16; ++level) {
    const uint32_t lw = std::max(s.width >> level, 1u), lh = std::max(s.height >> level, 1u);
    const uint32_t bx = (lw + 3) / 4, by = (lh + 3) / 4;
    const bool in_tail = packed && level >= packed_level;
    const uint32_t stored = in_tail ? packed_level : level;   // S108 CAP2: 0 when mip 0 itself is in the tail: the mips' own tail at the mip address, offset 0, laid out like a level 0 with mip
                                                              // strides (next_pow2(w); SDK util.cpp GetGuestTextureLayout "Loop iteration 1 - mips packed mip tail")
    const MipLayout l = LevelLayoutOf(s, stored);
    uint32_t ox = 0, oy = 0;
    if (in_tail) TailOffset(s.width, s.height, s, level, ox, oy);
    const uint32_t region = mip_addr + offs[stored];
    uint32_t top = 0;
    if (s.tiled) {
      for (uint32_t y = 0; y < by; ++y)
        for (uint32_t x = 0; x < bx; ++x) top = std::max(top, TiledOffset2D(int32_t(ox + x), int32_t(oy + y), l.pitch_blocks, log2_bpb) + s.bpb);
    } else {
      top = (oy + by - 1) * l.row_bytes + (ox + bx) * s.bpb;
    }
    if (!top || top > kMaxReadHard) return Why::Size;
    src.assign(top, 0);
    if (!phys(region, top, src.data())) return Why::Memory;
    const size_t at = out.data.size();
    out.data.resize(at + size_t(bx) * by * s.bpb);
    for (uint32_t y = 0; y < by; ++y)
      for (uint32_t x = 0; x < bx; ++x) {
        const uint32_t o = s.tiled ? TiledOffset2D(int32_t(ox + x), int32_t(oy + y), l.pitch_blocks, log2_bpb) : (oy + y) * l.row_bytes + (ox + x) * s.bpb;
        uint8_t* d = out.data.data() + at + (size_t(y) * bx + x) * s.bpb;
        std::memcpy(d, src.data() + o, s.bpb);
        SwapBlock(d, s.bpb, s.endian);
      }
    ++out.mips;
  }
  return Why::Ok;
}

}  // namespace rdr_scene::live::guest_tex
