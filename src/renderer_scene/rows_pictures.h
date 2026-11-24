// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstddef>
#include <cstdint>

namespace rdr_scene {

// layout / a bad size drops THIS section only (c_errors). One row per distinct key a shown prop row names.
inline constexpr uint32_t kScCrowdPictures = 48;
inline constexpr uint8_t kCrowdPictureLayout = 1;
inline constexpr uint32_t kCrowdPictureBytes = 4096;   // the biggest picture (the 64x128 foam hand, DXT1)
enum CrowdPictureFormat : uint32_t { kCpfBc1 = 18, kCpfBc2 = 19, kCpfBc3 = 20 };   // the game's format codes (the adapter's FormatOf)
struct CrowdPictureRow {
  uint32_t key = 0;          // the texture key word (CrowdPropRow::picture)
  uint16_t width = 0, height = 0;
  uint32_t format = 0;       // CrowdPictureFormat
  uint32_t nbytes = 0;       // bytes used of \bytes: ((width + 3) / 4) * ((height + 3) / 4) * (8 or 16)
  uint8_t layout = kCrowdPictureLayout, pad[3] = {};
  uint8_t bytes[kCrowdPictureBytes] = {};
};
static_assert(sizeof(CrowdPictureRow) == 4116, "CrowdPictureRow is a flat POD table");
// A row is valid only for a block-compressed format the renderer knows and EXACTLY the bytes of its size (the upload copies the image's full size: a short row would be read past its end).
inline bool CrowdPictureValid(const CrowdPictureRow& r) {
  if (r.layout != kCrowdPictureLayout || !r.width || !r.height || r.width > 4096 || r.height > 4096) return false;
  if (r.format != kCpfBc1 && r.format != kCpfBc2 && r.format != kCpfBc3) return false;
  const uint32_t bpb = r.format == kCpfBc1 ? 8u : 16u;
  return r.nbytes <= kCrowdPictureBytes && r.nbytes == ((uint32_t(r.width) + 3) / 4) * ((uint32_t(r.height) + 3) / 4) * bpb;
}

// S105 A12: the liquid items' pictures (section 53) get their OWN row, sized for the biggest blood decal (256x256 DXT1 = 32 KB; the blood textures of chCommon.pac PACH 0 entry 10 are 64x128 .. 256x256, 4-32 KB). The crowd's row
// (CrowdPictureRow, section 48) and every crowd reader stay as they were: the 4096-byte cap is the foam hand's, never a limit of the 360.
inline constexpr uint8_t kLiquidPictureLayout = 1;
inline constexpr uint32_t kLiquidPictureBytes = 32768;   // 256x256 DXT1
struct LiquidPictureRow {
  uint32_t key = 0;          // the texture key word (LiquidItemRow::picture = the item's wrapper address)
  uint16_t width = 0, height = 0;
  uint32_t format = 0;       // CrowdPictureFormat
  uint32_t nbytes = 0;       // bytes used of \bytes: ((width + 3) / 4) * ((height + 3) / 4) * (8 or 16)
  uint8_t layout = kLiquidPictureLayout, pad[3] = {};
  uint32_t hash[2] = {};     // PictureHashOf (width, height, format, bytes): made ONCE when the hook reads the texture; the picture cache compares it instead of hashing 32 KB a frame (review F3)
  uint8_t bytes[kLiquidPictureBytes] = {};
};
static_assert(sizeof(LiquidPictureRow) == 32796, "LiquidPictureRow is a flat POD table");
inline uint64_t PictureHashOf(uint16_t width, uint16_t height, uint32_t format, const uint8_t* bytes, size_t n) {   // FNV-1a over the header words and the blocks (the cache's own rule)
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](const uint8_t* p, size_t k) { for (size_t i = 0; i < k; ++i) h = (h ^ p[i]) * 1099511628211ull; };
  const uint32_t head[3] = {width, height, format};
  mix(reinterpret_cast<const uint8_t*>(head), sizeof head), mix(bytes, n);
  return h;
}
inline void LiquidPictureSeal(LiquidPictureRow& r) {   // sets hash[] from the row's own fields
  const uint64_t h = PictureHashOf(r.width, r.height, r.format, r.bytes, r.nbytes);
  r.hash[0] = uint32_t(h), r.hash[1] = uint32_t(h >> 32);
}
inline bool LiquidPictureValid(const LiquidPictureRow& r) {
  if (r.layout != kLiquidPictureLayout || !r.width || !r.height || r.width > 4096 || r.height > 4096) return false;
  if (r.format != kCpfBc1 && r.format != kCpfBc2 && r.format != kCpfBc3) return false;
  const uint32_t bpb = r.format == kCpfBc1 ? 8u : 16u;
  return r.nbytes <= kLiquidPictureBytes && r.nbytes == ((uint32_t(r.width) + 3) / 4) * ((uint32_t(r.height) + 3) / 4) * bpb;
}
// One name for the picture cache (crowd_picture_cache.h): each row type is valid by its own rule.
inline bool PictureValid(const CrowdPictureRow& r) { return CrowdPictureValid(r); }
inline bool PictureValid(const LiquidPictureRow& r) { return LiquidPictureValid(r); }
inline uint64_t PictureHash(const CrowdPictureRow& r) { return PictureHashOf(r.width, r.height, r.format, r.bytes, r.nbytes); }
inline uint64_t PictureHash(const LiquidPictureRow& r) { return uint64_t(r.hash[0]) | (uint64_t(r.hash[1]) << 32); }   // sealed at the read (LiquidPictureSeal), never recomputed per frame

}  // namespace rdr_scene
