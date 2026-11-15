// The window's swapchain. Native orientation always: on a rotated phone the compositor does no work, the final draw rotates instead.
#pragma once
#include <string>
#include <vector>

#include "api.h"
#include "device.h"
#include "exclusive_policy.h"  // S102 G2b
#include "surface_lifecycle.h"

namespace rdr {

class Swapchain {
 public:
  bool Create(Device* device, const NativeWindow& window, bool vsync, std::string& error);
  // S102 G2 (measurement aid): no window, no display: three images of w x h that the present draws into and nothing shows. Acquire / Present never wait.
  // rplay --present-size WxH uses it, so the output pass can be timed (the 'output' stamp role) at any panel size without a display.
  bool CreateOffscreen(Device* device, uint32_t w, uint32_t h, std::string& error);
  bool offscreen() const { return offscreen_; }
  const char* lost_at() const { return lost_at_; }  // S102 D1: the call that returned VK_ERROR_DEVICE_LOST (null: none)
  // S102 G2 (b): the output cap. NoteOutputCap(h) goes BEFORE Acquire (a window swapchain needs TRANSFER_DST for the blit: it is rebuilt once when a cap is first asked);
  // ApplyOutputCap(h, plant) goes AFTER a good Acquire and decides whether this frame draws at the capped size (extent() is then the capped one and
  // framebuffer() its target); RecordCapBlit(cmd, i) follows the pass and stretches the capped picture over the real image with a bilinear blit (Vulkan on
  // Windows cannot make the swapchain smaller than the window, so the stretch is ours). plant = TEST ONLY: the cap is asked for and never applied.
  void NoteOutputCap(uint32_t cap_h);
  bool ApplyOutputCap(uint32_t cap_h, bool plant);
  bool capped() const { return capped_; }
  void RecordCapBlit(VkCommandBuffer cmd, uint32_t image_index);
  VkExtent2D panel_extent() const { return extent_; }  // the real image's size, whatever the cap
  // Offscreen only: the last image drawn, as RGBA8 (waits for the device). False when nothing was drawn or this is a window.
  bool ReadLast(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);
  void Destroy();
  // S102 G2b: wwe2k14_rdr_exclusive_fullscreen. Called once before Create(); Windows only (elsewhere the state is "off"). See exclusive_policy.h for the states.
  void ConfigureExclusive(bool wanted);

  // False when there is nothing to draw to right now (minimised). Rebuilds the swapchain itself if it went out of date.
  bool Acquire(VkSemaphore available, uint32_t& image_index);
  // desired_ns: the earliest time (CLOCK_MONOTONIC ns) to show it (VK_GOOGLE_display_timing); 0 = as soon as the mode allows.
  // Lane 256: true only when the present was accepted (VK_SUCCESS or VK_SUBOPTIMAL_KHR, as the SDK presenter counts it).
  bool Present(uint32_t image_index, VkSemaphore finished, uint64_t desired_ns = 0);
  // Lane 235: the app's background / foreground events (any thread; they only set flags the next Acquire reads).
  void OnBackground() { life_.OnBackground(); }
  void OnForeground() { life_.OnForeground(); }
  uint64_t refresh_ns() const { return refresh_ns_; }  // the display's refresh duration (display timing), 0 = unknown

  VkRenderPass render_pass();  // S102 G2: the transfer-src one while capped or offscreen (compatible with the window's: framebuffers and pipelines are shared)
  VkRenderPass render_pass_noclear();  // S102 G1: compatible with render_pass(), loadOp DONT_CARE (the caller clears the bars); made on first use only
  VkFramebuffer framebuffer(uint32_t index) const { return capped_ ? cap_[index].framebuffer : framebuffers_[index]; }
  VkFormat format() const { return format_; }
  VkExtent2D extent() const { return capped_ ? draw_extent_ : extent_; }  // the size the present draws at (S102 G2: the capped one while capped)
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
  VkRenderPass render_pass_noclear_ = VK_NULL_HANDLE;  // S102 G1 (lazy)
  VkRenderPass MakePass(VkAttachmentLoadOp load, VkImageLayout final_layout);
  VkRenderPass render_pass_xfer_ = VK_NULL_HANDLE, render_pass_xfer_noclear_ = VK_NULL_HANDLE;  // S102 G2
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
  const char* lost_at_ = nullptr;  // S102 D1
  // S102 G2b
  excl::Policy excl_;
  bool excl_wanted_ = false;                   // ConfigureExclusive(): the switch, applied to the policy in Create() once the device is known
  void* acquire_fn_ = nullptr;                 // F5: vkAcquire / vkReleaseFullScreenExclusiveModeEXT, loaded in Create() when the device has the extension
  void* release_fn_ = nullptr;
  bool excl_logged_ = false;                   // the start line was printed (later changes print their own)
  void QueryExclusiveSupport();                // F3, F5
  void AcquireExclusive();                     // after a swapchain made with the structs: vkAcquireFullScreenExclusiveModeEXT, then the state and its line
  void ReleaseExclusive(VkSwapchainKHR swapchain);  // vkReleaseFullScreenExclusiveModeEXT when held
  void LogExclusive();
  // S102 G2
  struct CapTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };
  void ReleaseCap();
  bool MakeCap(uint32_t w, uint32_t h);
  VkDeviceMemory BindNew(VkImage image);
  std::vector<CapTarget> cap_;
  std::vector<VkDeviceMemory> own_memory_;     // offscreen: the images' memory (images_ holds the images)
  VkExtent2D draw_extent_{};
  bool capped_ = false;
  bool cap_wanted_ = false;                    // a cap was asked: swapchain images carry TRANSFER_DST
  bool dst_usage_ = false;                     // the current swapchain was made with TRANSFER_DST
  bool offscreen_ = false;
  bool blit_ok_ = false;                       // F8: the format can be blitted linearly (BLIT_SRC | BLIT_DST | SAMPLED_IMAGE_FILTER_LINEAR), cached in Build() / CreateOffscreen()
  uint32_t failed_cap_h_ = 0;                  // F7: a MakeCap that failed at this (cap, panel height) is not retried until either changes
  uint32_t failed_panel_h_ = 0;
  uint32_t next_offscreen_ = 0, last_offscreen_ = 0;
  bool drawn_offscreen_ = false;
  VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
};

}  // namespace rdr
