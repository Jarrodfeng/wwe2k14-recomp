#include "swapchain.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "log_formats.h"  // S101 lane 19 review: shared with the case zip
#include "problem.h"

namespace rdr {

using namespace vk;

namespace {

// The platform surface calls, declared here so windows.h and android headers stay out of the renderer.
struct Win32SurfaceInfo {
  VkStructureType type;
  const void* next;
  VkFlags flags;
  void* instance;
  void* window;
};
struct AndroidSurfaceInfo {
  VkStructureType type;
  const void* next;
  VkFlags flags;
  void* window;
};
using CreateSurfaceFn = VkResult(VKAPI_PTR*)(VkInstance, const void*, const VkAllocationCallbacks*, VkSurfaceKHR*);

}  // namespace

bool Swapchain::MakeSurface(const NativeWindow& window, std::string& error) {
#if defined(_WIN32)
  Win32SurfaceInfo info{VkStructureType(1000009000) /* WIN32_SURFACE_CREATE_INFO_KHR */, nullptr, 0, window.instance, window.window};
  auto create = reinterpret_cast<CreateSurfaceFn>(vk::GetInstanceProc(device_->instance(), "vkCreateWin32SurfaceKHR"));
#else
  AndroidSurfaceInfo info{VkStructureType(1000008000) /* ANDROID_SURFACE_CREATE_INFO_KHR */, nullptr, 0, window.window};
  auto create = reinterpret_cast<CreateSurfaceFn>(vk::GetInstanceProc(device_->instance(), "vkCreateAndroidSurfaceKHR"));
#endif
  if (!create || create(device_->instance(), &info, nullptr, &surface_) != VK_SUCCESS) {
    surface_ = VK_NULL_HANDLE;
    error = "Could not attach to the game window.";
    return false;
  }
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(device_->physical(), device_->queue_family(), surface_, &supported);
  if (!supported) {
    error = "The graphics queue cannot present to this window.";
    return false;
  }
  return true;
}

// Lane 235: let go of the window's surface (background, or the window is gone). Nothing may still use the images.
void Swapchain::ReleaseSurface() {
  VkDevice dev = device_->device();
  vkDeviceWaitIdle(dev);
  ReleaseImages();
  if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  if (surface_) vkDestroySurfaceKHR(device_->instance(), surface_, nullptr);
  surface_ = VK_NULL_HANDLE;
  extent_ = {};
  stale_ = false;
}

bool Swapchain::Create(Device* device, const NativeWindow& window, bool vsync, std::string& error) {
  device_ = device;
  window_ = window;
  life_.SetWaitForEventOnLoss(window.current != nullptr);
  // Lane 235 review SF2: the handle cached at set-up may be freed by now (home and back during the boot): ask for the current one.
  void* const first_window = window.current ? window.current(window.current_user) : window.window;
  if (window.current && (!first_window || WindowEvents().background())) {
    error = "The game window is not ready (the app is in the background).";
    return false;
  }
  NativeWindow first = window;
  first.window = first_window;
  window_ = window;
  if (!MakeSurface(first, error)) return false;

  uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device->physical(), surface_, &format_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(device->physical(), surface_, &format_count, formats.data());
  // Plain 8-bit UNORM: the display curve is applied by our final draw, so the format must not apply one.
  format_ = VK_FORMAT_UNDEFINED;
  for (const VkSurfaceFormatKHR& candidate : formats) {
    if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM) {
      format_ = candidate.format;
      color_space_ = candidate.colorSpace;
      break;
    }
  }
  if (format_ == VK_FORMAT_UNDEFINED) {
    error = "This display offers no plain 8-bit format.";
    return false;
  }

  uint32_t mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device->physical(), surface_, &mode_count, nullptr);
  std::vector<VkPresentModeKHR> modes(mode_count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(device->physical(), surface_, &mode_count, modes.data());
  present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync) {
    for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
      if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) {
        present_mode_ = wanted;
        break;
      }
    }
  }

  VkAttachmentDescription attachment{};
  attachment.format = format_;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass_info.attachmentCount = 1;
  pass_info.pAttachments = &attachment;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  pass_info.dependencyCount = 1;
  pass_info.pDependencies = &dependency;
  vkCreateRenderPass(device->device(), &pass_info, nullptr, &render_pass_);

  Build();
  life_.Created(first_window, true);
  return true;
}

void Swapchain::ReleaseImages() {
  VkDevice dev = device_->device();
  for (VkFramebuffer framebuffer : framebuffers_) vkDestroyFramebuffer(dev, framebuffer, nullptr);
  for (VkImageView view : views_) vkDestroyImageView(dev, view, nullptr);
  for (VkSemaphore semaphore : finished_) vkDestroySemaphore(dev, semaphore, nullptr);
  framebuffers_.clear();
  views_.clear();
  finished_.clear();
  images_.clear();
}

bool Swapchain::Build() {
  VkDevice dev = device_->device();
  vkDeviceWaitIdle(dev);
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical(), surface_, &caps);
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == 0xFFFFFFFFu) extent = {caps.minImageExtent.width, caps.minImageExtent.height};
  const VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
  transform_ = transform;
  quarter_turns_ = transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2 : transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3 : 0;
  if (quarter_turns_ & 1) std::swap(extent.width, extent.height);
  ReleaseImages();
  VkSwapchainKHR old = swapchain_;
  swapchain_ = VK_NULL_HANDLE;
  extent_ = extent;
  stale_ = false;
  if (extent.width == 0 || extent.height == 0) {
    if (old) vkDestroySwapchainKHR(dev, old, nullptr);
    return false;
  }
  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = surface_;
  info.minImageCount = std::max(caps.minImageCount, 3u);
  if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
  info.imageFormat = format_;
  info.imageColorSpace = color_space_;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = transform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (VkCompositeAlphaFlagBitsKHR option : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
    if (caps.supportedCompositeAlpha & option) {
      info.compositeAlpha = option;
      break;
    }
  }
  info.presentMode = present_mode_;
  info.clipped = VK_TRUE;
  info.oldSwapchain = old;
  if (vkCreateSwapchainKHR(dev, &info, nullptr, &swapchain_) != VK_SUCCESS) {
    Problem("swapchain_create_failed", "%ux%u", extent.width, extent.height);
    swapchain_ = VK_NULL_HANDLE;
    if (old) vkDestroySwapchainKHR(dev, old, nullptr);
    return false;
  }
  refresh_ns_ = 0;  // S100: the refresh duration, for timed presents (VK_GOOGLE_display_timing)
  if (device_->caps().display_timing && vkGetRefreshCycleDurationGOOGLE) {
    VkRefreshCycleDurationGOOGLE refresh{};
    if (vkGetRefreshCycleDurationGOOGLE(dev, swapchain_, &refresh) == VK_SUCCESS) refresh_ns_ = refresh.refreshDuration;
    Log(RDR_LOG_DISPLAY_TIMING_FMT, double(refresh_ns_) / 1e6);
  }
  if (old) vkDestroySwapchainKHR(dev, old, nullptr);
  uint32_t count = 0;
  vkGetSwapchainImagesKHR(dev, swapchain_, &count, nullptr);
  images_.resize(count);
  vkGetSwapchainImagesKHR(dev, swapchain_, &count, images_.data());
  views_.resize(count);
  framebuffers_.resize(count);
  finished_.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = images_[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &view, nullptr, &views_[i]);
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = render_pass_;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &views_[i];
    framebuffer.width = extent.width;
    framebuffer.height = extent.height;
    framebuffer.layers = 1;
    vkCreateFramebuffer(dev, &framebuffer, nullptr, &framebuffers_[i]);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev, &semaphore, nullptr, &finished_[i]);
  }
  Log(RDR_LOG_SWAPCHAIN_FMT, extent.width, extent.height, count, quarter_turns_, int(present_mode_));
  return true;
}

bool Swapchain::Acquire(VkSemaphore available, uint32_t& image_index) {
  // Lane 235: the window's surface lifecycle (Android background / foreground, a lost surface). Without a `current` source the one
  // window never changes (PC), so only a lost surface can lead to a re-create there.
  void* current = window_.current ? window_.current(window_.current_user) : window_.window;
  switch (life_.Decide(current)) {
    case SurfaceLifecycle::Action::kSkip:
      if (WindowEvents().background()) std::this_thread::sleep_for(std::chrono::milliseconds(16));  // nothing to draw: do not spin
      return false;
    case SurfaceLifecycle::Action::kRelease:
      Log("[rdr] swapchain: window surface released");
      ReleaseSurface();
      life_.Released();
      return false;
    case SurfaceLifecycle::Action::kCreate: {
      ReleaseSurface();
      NativeWindow now = window_;
      now.window = current;
      std::string error;
      bool ok = MakeSurface(now, error);
      if (ok) {
        // The render pass and the pipelines built against it assume this format: the new surface must offer it too.
        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical(), surface_, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical(), surface_, &format_count, formats.data());
        ok = false;
        for (const VkSurfaceFormatKHR& candidate : formats) ok = ok || (candidate.format == format_ && candidate.colorSpace == color_space_);
      }
      if (ok) ok = Build();
      if (!ok) {
        Problem("swapchain_recreate_failed", "%s", error.empty() ? "format or swapchain" : error.c_str());
        if (!swapchain_) {  // no half-made surface kept: the next try starts clean
          if (surface_) vkDestroySurfaceKHR(device_->instance(), surface_, nullptr);
          surface_ = VK_NULL_HANDLE;
        }
      }
      Log("[rdr] swapchain: window surface re-created on the current window: %s", ok ? "ok" : "failed");
      life_.Created(current, ok && swapchain_ != VK_NULL_HANDLE);
      if (!ok) return false;
      break;
    }
    case SurfaceLifecycle::Action::kPresent:
      break;
  }
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical(), surface_, &caps);
  VkExtent2D wanted = caps.currentExtent;
  if (wanted.width != 0xFFFFFFFFu && (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) std::swap(wanted.width, wanted.height);
  if (stale_ || !swapchain_ || caps.currentTransform != transform_ || (wanted.width != 0xFFFFFFFFu && (wanted.width != extent_.width || wanted.height != extent_.height))) {
    if (!Build()) return false;
  }
  VkResult result = vkAcquireNextImageKHR(device_->device(), swapchain_, UINT64_MAX, available, VK_NULL_HANDLE, &image_index);
  if (result == VK_ERROR_SURFACE_LOST_KHR) {  // lane 235: a new surface, not a stall
    life_.Lost();
    return false;
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    stale_ = true;
    return false;
  }
  if (result == VK_SUBOPTIMAL_KHR) stale_ = true;  // this image is still fine to use; rebuild for the next one
  else if (result != VK_SUCCESS) {
    Problem("swapchain_acquire_failed", "VkResult %d", int(result));
    return false;
  }
  return true;
}

bool Swapchain::Present(uint32_t image_index, VkSemaphore finished, uint64_t desired_ns) {
  VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  info.waitSemaphoreCount = 1;
  info.pWaitSemaphores = &finished;
  info.swapchainCount = 1;
  info.pSwapchains = &swapchain_;
  info.pImageIndices = &image_index;
  VkPresentTimeGOOGLE time{++present_id_, desired_ns};
  VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
  times.swapchainCount = 1;
  times.pTimes = &time;
  if (desired_ns && device_->caps().display_timing) info.pNext = &times;
  const VkResult result = vkQueuePresentKHR(device_->queue(), &info);
  if (result == VK_ERROR_SURFACE_LOST_KHR) life_.Lost();  // lane 235: the next Acquire makes a new surface
  else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) stale_ = true;
  else if (result != VK_SUCCESS) Problem("swapchain_present_failed", "VkResult %d", int(result));
  // S100 phone 120 fix (phone check s100ph3: the swapchain was made at the panel's 60 Hz, before the game's 120 Hz request, and kept
  // 16.58 ms): about once a second the refresh is read again; a change (the panel switched) rebuilds the swapchain at the new rate, as the
  // old presenter did ("the panel's rate can change underneath").
  if (device_->caps().display_timing && vkGetRefreshCycleDurationGOOGLE && present_id_ % 60 == 0) {
    VkRefreshCycleDurationGOOGLE refresh{};
    if (vkGetRefreshCycleDurationGOOGLE(device_->device(), swapchain_, &refresh) == VK_SUCCESS && refresh.refreshDuration &&
        (refresh.refreshDuration * 100 < refresh_ns_ * 97 || refresh.refreshDuration * 97 > refresh_ns_ * 100)) {
      Log(RDR_LOG_DISPLAY_TIMING_NOW_FMT, double(refresh.refreshDuration) / 1e6, double(refresh_ns_) / 1e6);
      refresh_ns_ = refresh.refreshDuration;
      stale_ = true;
    }
  }
  return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;  // lane 256: accepted, as the SDK presenter counts it
}

void Swapchain::Destroy() {
  if (!device_) return;
  VkDevice dev = device_->device();
  ReleaseImages();
  if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
  if (render_pass_) vkDestroyRenderPass(dev, render_pass_, nullptr);
  if (surface_) vkDestroySurfaceKHR(device_->instance(), surface_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  render_pass_ = VK_NULL_HANDLE;
  surface_ = VK_NULL_HANDLE;
}

}  // namespace rdr
