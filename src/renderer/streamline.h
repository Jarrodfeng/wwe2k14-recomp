// Settings parity S10b (Session 100, Jarrod 2026-11-11 09:3x: "Let's focus on DLSS work now!"): NVIDIA DLSS / DLAA in the new renderer,
// through NVIDIA Streamline (the player's runtime folder: sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll; never downloaded).
// Our own module; the SDK's (thirdparty\rexglue-sdk-src\src\ui\vulkan\streamline.cpp, Session 18 / 24) is the reference for the calls.
// Manual hooking: the whole Vulkan instance and device go through the interposer's vkGetInstanceProcAddr, which adds what DLSS needs.
// Windows only, and only with RDR_HAS_STREAMLINE (the game build); elsewhere every call answers "not available".
#pragma once
#include <cstdint>
#include <string>

#include "vk_api.h"

namespace rdr::streamline {

// Before the Vulkan instance: loads the runtime from `dir` (NVIDIA's signature checked), slInit with DLSS. The interposer's
// vkGetInstanceProcAddr, or null (no folder, not signed, failed: logged).
PFN_vkGetInstanceProcAddr Init(const std::string& dir);
bool Active();
// After the device exists: whether DLSS runs on it (logged once).
bool Available(VkPhysicalDevice physical_device);
void Shutdown();

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
  VkImageCreateFlags flags = 0;
  VkImageUsageFlags usage = 0;
};

// One evaluation. Inputs width x height (render size), output out_width x out_height. Matrices row-major as Streamline wants them
// (row vectors: v' = v * M).
struct Frame {
  uint32_t frame_index = 0;
  uint32_t width = 0, height = 0, out_width = 0, out_height = 0;
  int mode = 0;  // 0 DLAA, 1 max performance, 2 balanced, 3 max quality, 4 ultra performance, 5 ultra quality
  Image color_in, depth, motion, color_out;
  float view_to_clip[16], clip_to_view[16], clip_to_prev_clip[16], prev_clip_to_clip[16];
  float jitter[2] = {};      // render pixels
  float mvec_scale[2] = {};  // multiplies the motion vectors (render pixels) into normalised units
  bool reset = false;
};

// Records the evaluation into `command_buffer` (outside a render pass). False + `reason` on failure.
bool Evaluate(VkCommandBuffer command_buffer, const Frame& frame, std::string* reason);

}  // namespace rdr::streamline
