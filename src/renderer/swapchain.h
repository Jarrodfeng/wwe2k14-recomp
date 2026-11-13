// The window's swapchain. Native orientation always: on a rotated phone the compositor does no work, the final draw rotates instead.
#pragma once
#include <string>
#include <vector>

#include "api.h"
#include "device.h"
#include "surface_lifecycle.h"

namespace rdr {

class Swapchain {
 public:
  bool Create(Device* device, const NativeWindow& window, bool vsync, std::string& error);
  void Destroy();

  // False when there is nothing to draw to right now (minimised). Rebuilds the swapchain itself if it went out of date.
  bool Acquire(VkSemaphore available, uint32_t& image_index);
  // desired_ns: the earliest time (CLOCK_MONOTONIC ns) to show it (VK_GOOGLE_display_timing); 0 = as soon as the mode allows.
  // Lane 256: true only when the present was accepted (VK_SUCCESS or VK_SUBOPTIMAL_KHR, as the SDK presenter counts it).
  bool Present(uint32_t image_index, VkSemaphore finished, uint64_t desired_ns = 0);
  // Lane 235: the app's background / foreground events (any thread; they only set flags the next Acquire reads).
  void OnBackground() { life_.OnBackground(); }
  void OnForeground() { life_.OnForeground(); }
  uint64_t refresh_ns() const { return refresh_ns_; }  // the display's refresh duration (display timing), 0 = unknown

  VkRenderPass render_pass() const { return render_pass_; }
  VkFramebuffer framebuffer(uint32_t index) const { return framebuffers_[index]; }
  VkFormat format() const { return format_; }
  VkExtent2D extent() const { return extent_; }
  VkSemaphore finished_semaphore(uint32_t index) const { return finished_[index]; }
  // Quarter turns the final draw must apply so the picture is upright on the display.
  uint32_t quarter_turns() const { return quarter_turns_; }

 private:
  bool Build();
  bool MakeSurface(const NativeWindow& window, std::string& error);  // surface + queue support
  void ReleaseSurface();                                              // swapchain + images + surface (the render pass stays)
  void ReleaseImages();

  Device* device_ = nullptr;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkExtent2D extent_{};
  uint32_t quarter_turns_ = 0;
  uint64_t refresh_ns_ = 0;
  uint32_t present_id_ = 0;
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
  std::vector<VkSemaphore> finished_;
  NativeWindow window_{};
  SurfaceLifecycle life_{&WindowEvents()};
  bool stale_ = false;
  VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
};

}  // namespace rdr
