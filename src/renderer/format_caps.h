// S101 lane 200: format capabilities, one rule in one place (docs\renderer\FORMAT_CAPS.md).
// Vulkan makes most image-format features optional per device. The renderer asks the device once (Device::FormatHas) and every
// choke point that uses a format answers a missing feature the same way: linear sampling -> nearest, linear blit -> nearest blit,
// blending -> blend off for that attachment, vertex input -> logged. Nothing is substituted by a different format: the game copies
// bits between same-size formats (R32F depth texels, raw copies), so a different format would change values.
#pragma once
#include <cstdint>

#include "vk_api.h"

namespace rdr {

// The test switch (--wwe2k14_rdr_format_caps_mask=<hex>, dev only, default 0): pretends a feature is missing on this device so the
// fallback path runs on a full-feature PC. Each bit removes one feature from every format of its family.
enum FormatCapMask : uint32_t {
  kCapNoLinear32F = 1u << 0,       // R32 / RG32 / RGBA32 SFLOAT lose linear filtering (Adreno 640, GitHub #185)
  kCapNoBlend32F = 1u << 1,        // ... lose blending as a colour attachment
  kCapNoLinear16Norm = 1u << 2,    // R16 / RG16 / RGBA16 UNORM and SNORM lose linear filtering
  kCapNoLinear16F = 1u << 3,       // R16 / RG16 / RGBA16 SFLOAT lose linear filtering
  kCapNoBlend16F = 1u << 4,        // ... lose blending
  kCapNoBlendRgb10A2 = 1u << 5,    // A2B10G10R10 loses blending
  kCapFallbacksOff = 0x80000000u,  // the re-find: ask the driver for the unsupported thing anyway (counted, never for play)
};

inline bool IsFloat32Format(VkFormat f) { return f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT; }
inline bool IsFloat16Format(VkFormat f) { return f == VK_FORMAT_R16_SFLOAT || f == VK_FORMAT_R16G16_SFLOAT || f == VK_FORMAT_R16G16B16A16_SFLOAT; }
inline bool IsNorm16Format(VkFormat f) {
  return f == VK_FORMAT_R16_UNORM || f == VK_FORMAT_R16G16_UNORM || f == VK_FORMAT_R16G16B16A16_UNORM || f == VK_FORMAT_R16_SNORM || f == VK_FORMAT_R16G16_SNORM ||
         f == VK_FORMAT_R16G16B16A16_SNORM;
}

// `features` as the device reports them, minus what the test mask pretends is missing.
inline VkFormatFeatureFlags ApplyFormatCapMask(VkFormat format, VkFormatFeatureFlags features, uint32_t mask) {
  if (!mask) return features;
  if ((mask & kCapNoLinear32F) && IsFloat32Format(format)) features &= ~VkFormatFeatureFlags(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
  if ((mask & kCapNoBlend32F) && IsFloat32Format(format)) features &= ~VkFormatFeatureFlags(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
  if ((mask & kCapNoLinear16Norm) && IsNorm16Format(format)) features &= ~VkFormatFeatureFlags(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
  if ((mask & kCapNoLinear16F) && IsFloat16Format(format)) features &= ~VkFormatFeatureFlags(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
  if ((mask & kCapNoBlend16F) && IsFloat16Format(format)) features &= ~VkFormatFeatureFlags(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
  if ((mask & kCapNoBlendRgb10A2) && format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) features &= ~VkFormatFeatureFlags(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
  return features;
}

}  // namespace rdr
