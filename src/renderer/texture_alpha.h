// S102 lane C4 (docs\renderer\COLOUR_PASS.md "C4"): does a texture have an alpha channel that is 1 everywhere? Pure functions and one small table (no Vulkan, no cvar) so tools\renderer\c4_overdraw_test.cpp can prove them.
//
// The scene colour pass's arena draws multiply their output alpha by the diffuse texture's alpha (fx_bg.glsl: outColor.w = diffuse.w * inColor.w). G10's depth prepass needs that alpha to be 1 to treat
// the draw as opaque. A draw's slot-0 texture is provably alpha-free when ALL of these hold (SlotAlphaOne):
//   (0) the sampler cannot hand back a transparent border (review F3): no address mode is CLAMP_TO_BORDER, or the border colour is opaque (VK_BORDER_COLOR 2..5). This also guards swizzle ONE;
//   (1) the slot's view swizzle puts ONE into alpha (VK_COMPONENT_SWIZZLE_ONE = 2): the game's own "X" formats, no content needed; or
//   (2) the format has no alpha channel (R8, RG8, R5G6B5, BC4, BC5, 16 / 32 bit one and two channel formats) and the swizzle leaves alpha alone (identity 0 or A = 6); or
//   (3) the format has alpha and the swizzle leaves it alone, and the AlphaTable's scan of the image's CONTENT (every (mip, layer) pair of the image's record, each seen, none out of range; review F9) finds the
//       smallest alpha is 255: BC1 (punch-through blocks with an index 3), BC2 (4-bit alpha), BC3 (the 8-value ramp), RGBA8 / BGRA8 (byte 3).
// Anything else (a swizzle that routes a colour channel or ZERO into alpha, another format, content that is incomplete) is "unknown" and refuses the draw.
// The table only holds what the scan saw: a content that arrives with the scan off ERASES the entry (review F1), and so does a changed or released record (F2): "takes effect from the next load" (F7).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "api.h"

namespace rdr::texture_alpha {

constexpr int kUnknown = 256;  // the scan could not say (a format it does not read, a short block run)
constexpr uint8_t kSwizzleIdentity = 0, kSwizzleZero = 1, kSwizzleOne = 2, kSwizzleA = 6;
constexpr uint8_t kAddressClampToBorder = 3;  // VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER

// Formats whose texels have no alpha channel (a sample returns alpha 1 through an identity or A swizzle).
inline bool FormatHasNoAlpha(Format f) {
  switch (f) {
    case Format::R8: case Format::R8_SNORM: case Format::RG8: case Format::R5G6B5: case Format::RG16_UNORM: case Format::RG16_SNORM: case Format::R16_UNORM: case Format::RG16F:
    case Format::R16F: case Format::RG32F: case Format::R32F: case Format::BC4: case Format::BC5: case Format::EAC_R11: case Format::EAC_RG11:
      return true;
    default:
      return false;
  }
}
inline bool FormatScanned(Format f) { return f == Format::BC1 || f == Format::BC2 || f == Format::BC3 || f == Format::RGBA8 || f == Format::BGRA8; }

namespace detail {
// The 8-byte alpha block of BC3: the smallest value any of its 16 texels takes. Fast path (review F6): a0 = a1 = 255 is every ramp value 255.
inline int Bc3RampMin(const uint8_t* b) {
  const int a0 = b[0], a1 = b[1];
  if (a0 == 255 && a1 == 255) return 255;
  int ramp[8] = {a0, a1};
  if (a0 > a1) {
    for (int k = 1; k < 7; ++k) ramp[k + 1] = ((7 - k) * a0 + k * a1) / 7;
  } else {
    for (int k = 1; k < 5; ++k) ramp[k + 1] = ((5 - k) * a0 + k * a1) / 5;
    ramp[6] = 0, ramp[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
  int lowest = 255;
  for (int p = 0; p < 16; ++p) lowest = std::min(lowest, ramp[(bits >> (3 * p)) & 7]);
  return lowest;
}
}  // namespace detail

// The smallest alpha (0..255) of `bytes` bytes of texel / block data of format `f`, or kUnknown. Whole blocks / texels only: a short tail is "unknown". It STOPS at the first value under 255 (review F6): the
// caller only needs "is it 255", so the value returned for a texture with alpha is some value under 255, not necessarily the smallest.
inline int MinAlpha(Format f, const uint8_t* data, size_t bytes) {
  if (!data) return kUnknown;
  switch (f) {
    case Format::BC1:
      if (bytes % 8) return kUnknown;
      for (size_t at = 0; at < bytes; at += 8) {
        const uint8_t* b = data + at;
        const uint32_t c0 = uint32_t(b[0] | b[1] << 8), c1 = uint32_t(b[2] | b[3] << 8);
        if (c0 > c1) continue;  // four-colour block: every texel opaque
        const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
        for (int p = 0; p < 16; ++p)
          if (((idx >> (2 * p)) & 3) == 3) return 0;  // the transparent-black index of the three-colour block
      }
      return 255;
    case Format::BC2:
      if (bytes % 16) return kUnknown;
      for (size_t at = 0; at < bytes; at += 16)
        for (int k = 0; k < 8; ++k)
          if (data[at + k] != 0xFF) return std::min(data[at + k] & 15, data[at + k] >> 4) * 17;
      return 255;
    case Format::BC3:
      if (bytes % 16) return kUnknown;
      for (size_t at = 0; at < bytes; at += 16) {
        const int m = detail::Bc3RampMin(data + at);
        if (m < 255) return m;
      }
      return 255;
    case Format::RGBA8:
    case Format::BGRA8:
      if (bytes % 4) return kUnknown;
      for (size_t at = 3; at < bytes; at += 4)
        if (data[at] != 255) return data[at];
      return 255;
    default:
      return kUnknown;
  }
}

// What the table keeps per image: the smallest alpha over the image's supplied content (kUnknown when unscanned or incomplete), and the format scanned.
struct ImageAlpha {
  int lowest = kUnknown;
  Format format = Format::Undefined;
};

// A content's subresource, as the adapter delivers it (mip, layer, the packed block data).
struct SubIn {
  uint32_t mip = 0, layer = 0;
  const uint8_t* data = nullptr;
  size_t bytes = 0;
};
struct ScanStats {
  uint64_t bytes = 0, images = 0, ns = 0;  // what the scans read (census: MB and ms in the live line)
};

// Review F9: the scan of ONE content against the record's (mips, layers). Every subresource must be inside the record's range; each (mip, layer) pair is noted in a bitset and a duplicate or a missing pair makes
// the answer unknown (the content does not describe exactly the image's levels: the sampler could read a level the scan never saw, or two bytes' worth of one level).
inline ImageAlpha ScanSubresources(Format f, const SubIn* subs, size_t count, uint32_t mips, uint32_t layers, ScanStats* stats = nullptr) {
  ImageAlpha out;
  if (!FormatScanned(f) || !mips || !layers || count != size_t(mips) * layers) return out;
  std::vector<uint8_t> seen(size_t(mips) * layers, 0);  // the bitset of (mip, layer) pairs: checked BEFORE any data is read, so the early stop below never skips it
  for (size_t s = 0; s < count; ++s) {
    if (subs[s].mip >= mips || subs[s].layer >= layers) return out;
    uint8_t& flag = seen[size_t(subs[s].mip) * layers + subs[s].layer];
    if (flag) return out;  // a duplicated subresource (so another pair is missing)
    flag = 1;
  }
  int lowest = 255;
  for (size_t s = 0; s < count && lowest == 255; ++s) {
    if (stats) stats->bytes += subs[s].bytes;
    lowest = MinAlpha(f, subs[s].data, subs[s].bytes);  // stops at the first value under 255 (F6)
  }
  out.lowest = lowest, out.format = f;
  return out;
}

// Image records that can never be proven: a render target (written by the GPU), or a copy destination (storage follows another surface and is written by copies).
constexpr uint8_t kRecordUsageTarget = 2;  // rdr::kUsageTarget
constexpr uint8_t kRecordFlagCopyDest = 2;  // rdr_adapter::kImageCopyDest
inline bool RecordMayBeProven(uint8_t usage, uint8_t flags) { return !(usage & kRecordUsageTarget) && !(flags & kRecordFlagCopyDest); }

// The table the binder keeps: game image id -> the scan. OnContent is the ONLY writer (review F1): with the scan off, or for a record that may not be proven, it ERASES the entry, so a content change either
// rescans or invalidates. Erase is called on a record change and on a release (F2).
class AlphaTable {
 public:
  void OnContent(uint32_t image, bool scan_on, bool record_ok, Format f, uint32_t mips, uint32_t layers, const SubIn* subs, size_t count, ScanStats* stats = nullptr) {
    if (!scan_on || !record_ok) {
      if (!entries_.empty()) entries_.erase(image);
      return;
    }
    if (stats) ++stats->images;
    entries_[image] = ScanSubresources(f, subs, count, mips, layers, stats);
  }
  void Erase(uint32_t image) {
    if (!entries_.empty()) entries_.erase(image);
  }
  ImageAlpha Get(uint32_t image) const {
    const auto found = entries_.find(image);
    return found != entries_.end() ? found->second : ImageAlpha{};
  }
  size_t size() const { return entries_.size(); }

 private:
  std::unordered_map<uint32_t, ImageAlpha> entries_;
};

// The sampler's border rule (review F3): a draw whose sampler can return the border colour must have an opaque one (VK_BORDER_COLOR 2..5: FLOAT / INT_OPAQUE_BLACK, FLOAT / INT_OPAQUE_WHITE).
inline bool SamplerUsesBorder(const SamplerDesc& s) { return s.address_u == kAddressClampToBorder || s.address_v == kAddressClampToBorder || s.address_w == kAddressClampToBorder; }
inline bool SamplerBorderOpaque(const SamplerDesc& s) { return !SamplerUsesBorder(s) || (s.border >= 2 && s.border <= 5); }

// Is the slot's alpha provably 1 everywhere? `image` = the scan of the slot's image (may be a default ImageAlpha = unknown). This overload ignores the sampler (the older callers); use the next one.
inline bool SlotAlphaOne(Format image_format, uint8_t swizzle_a, const ImageAlpha& image) {
  if (swizzle_a == kSwizzleOne) return true;
  if (swizzle_a != kSwizzleIdentity && swizzle_a != kSwizzleA) return false;
  if (FormatHasNoAlpha(image_format)) return true;
  return image.format == image_format && image.lowest == 255;
}
inline bool SlotAlphaOne(Format image_format, uint8_t swizzle_a, const ImageAlpha& image, const SamplerDesc& sampler) {
  return SamplerBorderOpaque(sampler) && SlotAlphaOne(image_format, swizzle_a, image);
}

}  // namespace rdr::texture_alpha
