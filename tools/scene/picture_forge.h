// picture_forge.h: forged GUEST MEMORY for the picture source (S103 PN4: moved out of picture_source_unit.cpp so the host rig builds the same memory). Rig / unit only. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The handle registry at 0x8392DD9C, the wrapper, the library header, the six fetch dwords and the pixel bytes tiled and endian-swapped with the reader's own TiledOffset2D / SwapBlock (the I2 unit proved those
// against a written-out layout): what PictureSource::Seen reads at a row's first sight.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "../../src/scene_live/picture_source.h"

namespace picforge {
namespace ps = rdr_scene::live::picsrc;
namespace gt = rdr_scene::live::guest_tex;
namespace particles = rdr_scene::live::particles;

struct Forge {
  std::unordered_map<uint32_t, uint32_t> words;
  std::map<uint32_t, std::vector<uint8_t>> regions;   // physical base -> bytes
  mutable bool fail = false;
  uint32_t next_phys = 0x10000000u, n_obj = 0;
  static constexpr uint32_t kMgr = 0x40000000u, kEntries = 0x40010000u, kObjects = 0x50000000u;
  Forge() {
    words[particles::kTextureHandleGlobal] = kMgr;
    words[kMgr + 8] = kEntries;
    words[kMgr + 16] = kEntries + 8u * 0x10000u;
  }
  uint32_t u32(uint32_t a) const {
    if (a < 0x10000u) fail = true;
    const auto it = words.find(a);
    return it == words.end() ? 0u : it->second;
  }
  bool phys(uint32_t a, uint32_t n, uint8_t* dst) const {
    for (const auto& [base, bytes] : regions)
      if (a >= base && uint64_t(a) + n <= uint64_t(base) + bytes.size()) return std::memcpy(dst, bytes.data() + (a - base), n), true;
    return false;
  }
  uint32_t SlotOf(uint32_t key) const { return key & 0xFFFFu; }
  uint32_t KeyAt(uint32_t key) const {
    const auto it = words.find(kEntries + 8 * SlotOf(key));
    return it == words.end() ? 0 : it->second;
  }
  // Places one picture (mip 0 as `bytes`: BC blocks or 4-byte texels, as the reader decoded them) under `key`; replaces whatever the slot held.
  void Place(uint32_t key, rdr::Format fmt, uint32_t w, uint32_t h, const uint8_t* bytes, bool tiled, uint32_t endian, uint32_t fetch_swz = gt::kBgraIdentitySwizzle) {
    const bool bc = fmt == rdr::Format::BC1 || fmt == rdr::Format::BC2 || fmt == rdr::Format::BC3;
    const uint32_t bw = bc ? 4 : 1, bpb = fmt == rdr::Format::BC1 ? 8 : bc ? 16 : 4, log2 = bpb == 8 ? 3 : bpb == 4 ? 2 : 4;
    const uint32_t gf = fmt == rdr::Format::BC1 ? 18 : fmt == rdr::Format::BC2 ? 19 : fmt == rdr::Format::BC3 ? 20 : 6;
    const uint32_t bx = (w + bw - 1) / bw, by = (h + bw - 1) / bw;
    const uint32_t row_texels = (w + 31) & ~31u;
    const uint32_t pitch_blocks = ((row_texels + bw - 1) / bw + 31) & ~31u;
    uint32_t size = pitch_blocks * bpb * by;
    if (tiled) {
      size = 0;
      for (uint32_t y = 0; y < by; ++y)
        for (uint32_t x = 0; x < bx; ++x) size = std::max(size, gt::TiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, log2) + bpb);
    }
    std::vector<uint8_t> mem(size, 0);
    const int saved = gt::g_tex_plant;
    gt::g_tex_plant = 0;   // the game stored the picture with the TRUE swap and tiling, whatever plant is on
    for (uint32_t y = 0; y < by; ++y)
      for (uint32_t x = 0; x < bx; ++x) {
        uint8_t blk[16];
        std::memcpy(blk, bytes + (size_t(y) * bx + x) * bpb, bpb);
        gt::SwapBlock(blk, bpb, endian);
        const uint32_t off = tiled ? gt::TiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, log2) : (y * pitch_blocks + x) * bpb;
        std::memcpy(mem.data() + off, blk, bpb);
      }
    gt::g_tex_plant = saved;
    const uint32_t base = next_phys;
    next_phys += (size + 0xFFFu) & ~0xFFFu;
    next_phys += 0x1000u;
    regions[base] = std::move(mem);
    const uint32_t wrapper = kObjects + 0x100u * n_obj, header = wrapper + 0x80u;
    ++n_obj;
    words[wrapper] = particles::kWrapperVtableA, words[wrapper + particles::kWrapperKind] = 0, words[wrapper + particles::kWrapperTexture] = header;
    const uint32_t f0 = (tiled ? 1u << 31 : 0u) | ((row_texels / 32) << 22), f1 = gf | (endian << 6) | (wwe2k14::guest_view::ToEViewCpu(base) & 0xFFFFF000u), f2 = (w - 1) | ((h - 1) << 13), f5 = 1u << 9;
    const uint32_t f[6] = {f0, f1, f2, fetch_swz << 1, 0, f5};
    for (int i = 0; i < 6; ++i) words[header + particles::kHeaderFetch + 4 * i] = f[i];
    words[kEntries + 8 * SlotOf(key)] = key;
    words[kEntries + 8 * SlotOf(key) + 4] = wrapper;
  }
  // The slot is released (the game's death removed the entry).
  void Unplace(uint32_t key) { words.erase(kEntries + 8 * SlotOf(key)), words.erase(kEntries + 8 * SlotOf(key) + 4); }
};


}  // namespace picforge

