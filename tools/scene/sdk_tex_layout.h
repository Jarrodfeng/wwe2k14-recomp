// sdk_tex_layout: a 2D block-compressed texture laid out the way the console's memory holds it, computed by the SDK's OWN texture utilities (S108 lane CAP2).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The independent side of the tail-read proof (docs\research\s108\CAP2_plan.md, piece 2): tools\scene\sdk_tex_layout.cpp is compiled from thirdparty\rexglue-sdk-src
// (texture_util::GetGuestTextureLayout, GetPackedMipOffset, GetTiledOffset2D; rex::byte_swap), never from our reader (src\scene_live\guest_texture*.h). This header has no SDK include,
// so the unit (C++20, our headers) links against it without mixing the two.
#pragma once
#include <cstdint>
#include <vector>

namespace sdk_layout {

struct In {
  uint32_t width = 0, height = 0, mips = 1;   // texels; the levels held in `data`
  uint32_t gpu_format = 18;                    // 18 DXT1, 19 DXT2_3, 20 DXT4_5
  bool tiled = true, packed = true;            // the fetch's tiled bit and packed-mips bit
  uint32_t pitch_div32 = 0;                    // the fetch's base pitch (texels / 32); 0 = the smallest legal one for the width
  const uint8_t* data = nullptr;               // linear little-endian blocks, mip 0 first, each level tightly packed (assets::Texture::data)
  size_t size = 0;
};
struct Out {
  uint32_t pitch_div32 = 0;            // the pitch written into the fetch
  uint32_t packed_level = 0;           // GetPackedMipLevel (0 = mip 0 itself lives in the tail)
  uint32_t mip0_x = 0, mip0_y = 0;     // GetPackedMipOffset(level 0) in blocks (0, 0 when not packed)
  std::vector<uint8_t> base, mips;     // the bytes under base_address / mip_address, big-endian words (endian 8-in-32) as the console holds them
};
bool Lay(const In& in, Out& out);

}  // namespace sdk_layout
