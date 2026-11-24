// The particle namer's ContentHasher over guest texture memory (Session 103, lane F5). Pure: no game or SDK includes; the game build gives it the physical-memory reader.
// fetch dwords -> guest_texture.h's Shape (BC1 / BC2 / BC3, tiled bit, endian, base, pitch) -> mip 0 untiled and endian-swapped (ReadMip0, the adapter's own arithmetic) -> ContentKey with the
// format's fourcc ('DXT1' / 'DXT3' / 'DXT5'), the SAME function the NameIndex hashes the DDS files with (particle_namer.h ContentKey). A texture the shape code refuses (cube, volume, packed
// mip tail, not a multiple of 4, over 1 MB) is no key: the handle stays unnamed (counted no_key), never a guess.
#pragma once
#include <cstdint>
#include <vector>

#include "guest_texture.h"
#include "guest_texture_mips.h"   // S108 PRT
#include "particle_namer.h"

namespace rdr_scene::live::particles {

// `Phys` = bool(uint32_t addr, uint32_t n, uint8_t* dst): n bytes of pixel memory at the PHYSICAL address as stored.
template <class Phys>
class GuestContentHasher final : public ContentHasher {
 public:
  explicit GuestContentHasher(Phys phys) : phys_(std::move(phys)) {}
  bool MipZeroKey(const uint32_t fetch[6], uint64_t& key) const override {
    guest_tex::Shape s;
    if (guest_tex::ShapeOfFetch(fetch, s) != guest_tex::Why::Ok) return false;
    std::vector<uint8_t> blocks;
    if (guest_tex::ReadMip0(s, phys_, blocks) != guest_tex::Why::Ok) return false;
    const uint32_t fourcc = s.game_format == 18 ? 0x31545844u : s.game_format == 19 ? 0x33545844u : 0x35545844u;   // 'DXT1' / 'DXT3' / 'DXT5' (little-endian words, as the DDS header has them)
    key = ContentKey(s.width, s.height, fourcc, blocks.data(), blocks.size());
    return true;
  }
  bool DescribeKey(const uint32_t fetch[6], KeyInfo& out) const override {   // S103 PF: dev diagnostic, only on a namer miss
    guest_tex::Shape s;
    if (guest_tex::ShapeOfFetch(fetch, s) != guest_tex::Why::Ok) return false;
    std::vector<uint8_t> blocks;
    if (guest_tex::ReadMip0(s, phys_, blocks) != guest_tex::Why::Ok) return false;
    out.width = s.width, out.height = s.height, out.mip0_bytes = uint32_t(blocks.size());
    out.fourcc = s.game_format == 18 ? 0x31545844u : s.game_format == 19 ? 0x33545844u : 0x35545844u;
    for (size_t i = 0; i < 16 && i < blocks.size(); ++i) out.head[i] = blocks[i];
    return true;
  }
  // S108 PRT: the bound texture as the game hands it to the GPU, every stored level, through the same physical reader (its range and page checks: a refused read is a false, never a guess).
  bool ReadSprite(const uint32_t fetch[6], assets::Texture& out) const override {
    guest_tex::Chain c;
    if (guest_tex::ReadChain(fetch, phys_, c) != guest_tex::Why::Ok) return false;
    out = assets::Texture{};
    out.format = c.game_format == 18 ? assets::TexFormat::kBC1 : c.game_format == 19 ? assets::TexFormat::kBC2 : assets::TexFormat::kBC3;
    out.width = c.width, out.height = c.height, out.mips = c.mips;
    out.data = std::move(c.data);
    return true;
  }
 private:
  mutable Phys phys_;
};

}  // namespace rdr_scene::live::particles
