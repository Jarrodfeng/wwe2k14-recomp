// Instance, physical device and logical device. The device is created with the floor's features and nothing above them, on every machine.
#pragma once
#include <string>
#include <vector>

#include "vk_api.h"

namespace rdr {

struct DeviceOptions {
  int validation = 0;      // 0 off; 1 core + synchronisation; 2 also best practices
  bool windowed = false;   // false: headless, no surface or swapchain extensions
  int gpu_index = -1;      // -1: pick the best
  uint32_t format_caps_mask = 0;  // S101 lane 200: format_caps.h (dev switch; 0 = the device as it is)
  std::string streamline_dir;  // S10b: NVIDIA Streamline's runtime folder (DLSS); empty = plain Vulkan
};

// What the device offers within the floor. Capability answers only, never vendor names.
struct Caps {
  bool display_timing = false;  // VK_GOOGLE_display_timing enabled (present times; Android)
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  bool unified_memory = false;   // device-local memory is host-visible: write directly, no staging
  bool lazy_memory = false;      // transient attachments can be lazily allocated
  bool bc = false;
  bool astc_ldr = false;
  bool depth_clamp = false;
  bool anisotropy = false;
  float max_anisotropy = 1.0f;
  VkDeviceSize uniform_align = 256;
  VkDeviceSize atom_size = 64;
};

class Device {
 public:
  // On failure returns false and fills `error` with a sentence fit to show the player, naming what is missing.
  bool Create(const DeviceOptions& options, std::string& error);
  bool dlss() const { return dlss_; }  // S10b: Streamline loaded and DLSS available on this device
  void Destroy();

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queue_family() const { return queue_family_; }
  const Caps& caps() const { return caps_; }
  const VkPhysicalDeviceProperties& properties() const { return properties_; }
  const VkPhysicalDeviceMemoryProperties& memory() const { return memory_; }

  // Memory type with all of `required`, preferring also `preferred`. -1 if none.
  int FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const;
  bool FormatHas(VkFormat format, VkFormatFeatureFlags features) const;  // optimal tiling, with the test mask applied (format_caps.h)
  VkFormatFeatureFlags FormatFeatures(VkFormat format) const;
  bool VertexFormatReadable(VkFormat format) const;  // lane 200: bufferFeatures VERTEX_BUFFER (a different list from the image features)
  uint32_t format_caps_mask() const { return format_caps_mask_; }

  uint64_t validation_errors() const;
  bool debug_names() const { return messenger_ != VK_NULL_HANDLE; }  // debug utils enabled (validation on): objects may be named

 private:
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  bool dlss_ = false;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  Caps caps_;
  uint32_t format_caps_mask_ = 0;
  VkPhysicalDeviceProperties properties_{};
  VkPhysicalDeviceMemoryProperties memory_{};
};

}  // namespace rdr
