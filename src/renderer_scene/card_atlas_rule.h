// S103 CB1: the crowd card bake in ONE render pass. Pure: no GPU, no game, no SDK include (tools\scene\card_atlas_unit.cpp tests it).
//
// The game bakes each far-card picture (64 x 64, one per person type, up to 20) in its own render pass and resolves it into its own texture: the Xbox 360 renders into EDRAM one target set at a
// time and resolves out of it, so "one pass + one copy per picture" was the natural shape there [L: hardware knowledge; the game's loop is sub_827FD698 over the thumbnail container's 20 structs,
// docs\renderer\CARD_BAKE_OURS.md section 5]. On a PC / phone GPU each pass costs ~0.028 ms of begin / end overhead for 64 x 64 of work (docs\research\s103\GCOL.md C3): a 360 HABIT
// (docs\renderer\360_HABITS.md row 4). We bake every type into ONE atlas image in ONE pass: tile k = rows [k * kPitch + kGutter, + kTile), each drawn with its own viewport + scissor.
//
// What keeps the picture byte-identical to the per-type pictures:
//  - the atlas is exactly kTile wide, so the far-card sampler's clamp-to-edge works across u as before;
//  - across v the sampler's clamp is reproduced by a gutter row above and below each tile that holds a COPY of the tile's edge row. The copy is not a copy op: the same bake draw is drawn again with
//    the viewport moved one row (so the tile's edge row lands in the gutter row) and the scissor cut to that one row. Same triangles, same fragments, same depth: the same pixels;
//  - the far-card quad's v is remapped per tile (tile row 0 .. kTile maps to atlas row y0 .. y0 + kTile): sampling positions are the same texels at the same weights.
// With anisotropic filtering above 1x the sampler's taps can reach more than one texel past the edge: that setting keeps the per-type pictures (CardAtlasUsable).
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_scene {

// Bits of wwe2k14_rdr_card_atlas (stream-free build, read once at start; default 3 there, 0 in the stream build and the rigs):
//   1 = every card of a frame baked in one pass into one atlas image;  2 = a card whose bake inputs equal its last bake's is not baked again (needs 1).
inline constexpr int kCardAtlasOnePass = 1;
inline constexpr int kCardAtlasSkipSame = 2;
inline std::atomic<int> g_card_atlas{0};

inline constexpr uint32_t kAtlasTypes = 20;
inline constexpr uint32_t kAtlasTile = 64;
inline constexpr uint32_t kAtlasGutter = 1;
inline constexpr uint32_t kAtlasPitch = kAtlasTile + 2 * kAtlasGutter;
inline constexpr uint32_t kAtlasWidth = kAtlasTile;
inline constexpr uint32_t kAtlasHeight = kAtlasTypes * kAtlasPitch;

// The first row of tile `type`'s picture inside the atlas.
inline constexpr uint32_t AtlasTileY(uint32_t type) { return type * kAtlasPitch + kAtlasGutter; }
// The far-card quad's v for tile `type`: a v in [0, 1] of the per-type picture becomes this atlas v.
inline constexpr float AtlasV(uint32_t type, float v) { return (float(AtlasTileY(type)) + v * float(kAtlasTile)) / float(kAtlasHeight); }

// The atlas replaces the per-type pictures only where the sampler reads at most the one gutter row (anisotropy 1x: the shipped default).
inline constexpr bool CardAtlasUsable(uint32_t max_anisotropy) { return max_anisotropy <= 1; }

// FNV-style 64-bit mix used for the bake's input key (8 bytes at a time, like LiveCrowd::Impl::HashBytes).
inline uint64_t AtlasMix(uint64_t h, const void* d, uint32_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(d);
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w = 0;
    for (int k = 0; k < 8; ++k) w |= uint64_t(p[i + uint32_t(k)]) << (8 * k);
    h = (h ^ w) * 1099511628211ull;
    h ^= h >> 29;
  }
  for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h ^ (uint64_t(n) << 1);
}

}  // namespace rdr_scene
