// sdk_tex_layout.cpp: see sdk_tex_layout.h. Built with the SDK's own sources (tools\scene\car_paint_check.py compiles it at -std=c++23 with
// thirdparty\rexglue-sdk-src\include, src\graphics\pipeline\texture\util.cpp, info_formats.cpp and src\core\math_msvc.cpp). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "sdk_tex_layout.h"

#include <algorithm>
#include <cstring>

#include <rex/graphics/pipeline/texture/util.h>

namespace sdk_layout {
namespace tu = rex::graphics::texture_util;
namespace xenos = rex::graphics::xenos;

bool Lay(const In& in, Out& out) {
  out = Out();
  if (!in.width || !in.height || !in.mips || !in.data) return false;
  const xenos::TextureFormat fmt = xenos::TextureFormat(in.gpu_format);
  const rex::graphics::FormatInfo* fi = rex::graphics::FormatInfo::Get(in.gpu_format);
  const uint32_t bw = fi->block_width, bh = fi->block_height, bpb = fi->bytes_per_block();
  const uint32_t log2_bpb = rex::log2_floor(bpb);
  out.pitch_div32 = in.pitch_div32 ? in.pitch_div32 : (in.width + 31) / 32;
  if (!in.tiled && !in.pitch_div32) {   // a linear base row is 256-byte aligned (util.h: Direct3D 9 writes an already aligned pitch into the fetch)
    const uint32_t row_bytes = (in.width + bw - 1) / bw * bpb;
    const uint32_t aligned = (row_bytes + 255) / 256 * 256;
    out.pitch_div32 = (aligned / bpb * bw + 31) / 32;
  }
  const tu::TextureGuestLayout L = tu::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, out.pitch_div32, in.width, in.height, 1, in.tiled, fmt, in.packed, true, in.mips - 1);
  out.packed_level = L.packed_level;
  if (in.packed) {
    uint32_t z = 0;
    tu::GetPackedMipOffset(in.width, in.height, 1, fmt, 0, out.mip0_x, out.mip0_y, z);
  }
  size_t at = 0;
  for (uint32_t level = 0; level < in.mips; ++level) {
    const uint32_t lw = std::max(in.width >> level, 1u), lh = std::max(in.height >> level, 1u), bx = (lw + bw - 1) / bw, by = (lh + bh - 1) / bh;
    if (at + size_t(bx) * by * bpb > in.size) return false;
    const bool base = level == 0;
    const uint32_t stored = std::min(level, L.packed_level);
    const tu::TextureGuestLayout::Level& ll = base ? L.base : L.mips[stored];
    const uint32_t region = base ? 0 : L.mip_offsets_bytes[stored];
    uint32_t ox = 0, oy = 0, oz = 0;
    if (in.packed && level >= L.packed_level) tu::GetPackedMipOffset(in.width, in.height, 1, fmt, level, ox, oy, oz);
    const uint32_t pitch_blocks = ll.row_pitch_bytes / bpb;
    std::vector<uint8_t>& dst = base ? out.base : out.mips;
    for (uint32_t y = 0; y < by; ++y)
      for (uint32_t x = 0; x < bx; ++x) {
        const uint32_t off = region + (in.tiled ? uint32_t(tu::GetTiledOffset2D(int32_t(ox + x), int32_t(oy + y), pitch_blocks, log2_bpb))
                                                : (oy + y) * ll.row_pitch_bytes + (ox + x) * bpb);
        if (dst.size() < size_t(off) + bpb) dst.resize(size_t(off) + bpb, 0);
        const uint8_t* src = in.data + at + (size_t(y) * bx + x) * bpb;
        for (uint32_t w = 0; w + 4 <= bpb; w += 4) {   // endian 8-in-32: each 32-bit word of the block stored byte-reversed
          uint32_t v;
          std::memcpy(&v, src + w, 4);
          v = rex::byte_swap(v);
          std::memcpy(dst.data() + off + w, &v, 4);
        }
      }
    at += size_t(bx) * by * bpb;
  }
  return true;
}

}  // namespace sdk_layout
