#include "swapchain.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "log_formats.h"  // S101 lane 19 review: shared with the case zip
#include "problem.h"
#include "output_switches.h"  // S102 G2: g_out_blit_plant
#include "breadcrumbs.h"  // S102 D1: rdr::DeviceLost

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

#if defined(_WIN32)
// S102 G2b: VK_EXT_full_screen_exclusive, declared here (the platform header is kept out of the renderer, as for the surface above). Values from the registry:
// STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT 1000255000, ..._WIN32_INFO_EXT 1000255001; FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT 3.
struct ExclusiveInfo {  // VkSurfaceFullScreenExclusiveInfoEXT
  VkStructureType type;
  void* next;
  int full_screen_exclusive;
};
struct ExclusiveWin32Info {  // VkSurfaceFullScreenExclusiveWin32InfoEXT
  VkStructureType type;
  const void* next;
  void* hmonitor;
};
using ExclusiveFn = VkResult(VKAPI_PTR*)(VkDevice, VkSwapchainKHR);
// The three Win32 calls, bound by symbol name so no windows.h is needed (user32 and gdi32 are linked by the game and by rplay).
extern "C" void* rdr_MonitorFromWindow(void*, unsigned long) __asm__("MonitorFromWindow");
extern "C" void* rdr_GetDC(void*) __asm__("GetDC");
extern "C" int rdr_ReleaseDC(void*, void*) __asm__("ReleaseDC");
extern "C" int rdr_GetDeviceCaps(void*, int) __asm__("GetDeviceCaps");
#endif

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
  ReleaseExclusive(swapchain_);  // S102 G2b
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
#if defined(_WIN32)
  excl_.Configure(excl_wanted_, device->caps().full_screen_exclusive);  // S102 G2b
#else
  excl_.Configure(false, false);
#endif
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
#if defined(_WIN32)
  QueryExclusiveSupport();  // S102 G2b F3, F5
#endif

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

  render_pass_ = MakePass(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  Build();
  LogExclusive();  // S102 G2b: the one start line (also for off / unsupported)
  excl_logged_ = true;
  life_.Created(first_window, true);
  return true;
}

// ---- S102 G2b ----
void Swapchain::ConfigureExclusive(bool wanted) { excl_wanted_ = wanted; }

#if defined(_WIN32)
// F3 + F5: with the switch on and the extension enabled: load the two calls, and ask the surface whether fullScreenExclusive is supported for this window's monitor
// (vkGetPhysicalDeviceSurfaceCapabilities2KHR; the device layer only enables the extension when the instance listed VK_KHR_get_surface_capabilities2).
// A "not supported" answer, or no answer, is the normal path with the line `unsupported`.
void Swapchain::QueryExclusiveSupport() {
  if (!excl_.ChainStructs()) return;
  acquire_fn_ = reinterpret_cast<void*>(vkGetDeviceProcAddr(device_->device(), "vkAcquireFullScreenExclusiveModeEXT"));
  release_fn_ = reinterpret_cast<void*>(vkGetDeviceProcAddr(device_->device(), "vkReleaseFullScreenExclusiveModeEXT"));
  using Caps2Fn = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR*);
  const auto caps2 = reinterpret_cast<Caps2Fn>(vk::GetInstanceProc(device_->instance(), "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
  if (!acquire_fn_ || !release_fn_ || !caps2) {
    excl_.NoteSurfaceUnsupported();
    return;
  }
  ExclusiveInfo exclusive_info{VkStructureType(1000255000), nullptr, 3};
  ExclusiveWin32Info exclusive_win32{VkStructureType(1000255001), nullptr, rdr_MonitorFromWindow(window_.window, 2 /* MONITOR_DEFAULTTONEAREST */)};
  exclusive_info.next = &exclusive_win32;
  struct ExclusiveCaps {  // VkSurfaceCapabilitiesFullScreenExclusiveEXT
    VkStructureType type;
    void* next;
    VkBool32 supported;
  } out{VkStructureType(1000255002), nullptr, VK_FALSE};
  VkPhysicalDeviceSurfaceInfo2KHR surface_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
  surface_info.pNext = &exclusive_info;
  surface_info.surface = surface_;
  VkSurfaceCapabilities2KHR caps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
  caps.pNext = &out;
  if (caps2(device_->physical(), &surface_info, &caps) != VK_SUCCESS || !out.supported) excl_.NoteSurfaceUnsupported();
}
#endif

void Swapchain::LogExclusive() {
  uint32_t hz = 0;
#if defined(_WIN32)
  if (window_.window && (excl_.state() == excl::State::On || excl_.state() == excl::State::Lost)) {  // O1: off / unsupported read nothing from the display  // the panel's current refresh (GetDeviceCaps VREFRESH = 116): informative only
    if (void* dc = rdr_GetDC(window_.window)) {
      const int rate = rdr_GetDeviceCaps(dc, 116);
      rdr_ReleaseDC(window_.window, dc);
      hz = rate > 1 ? uint32_t(rate) : 0;
    }
  }
#endif
  char text[160];
  excl::Policy::Line(text, sizeof(text), excl_.state(), extent_.width, extent_.height, hz);
  Log("%s", text);
}

void Swapchain::AcquireExclusive() {
#if defined(_WIN32)
  const auto acquire = reinterpret_cast<ExclusiveFn>(acquire_fn_);
  const VkResult result = acquire ? acquire(device_->device(), swapchain_) : VK_ERROR_EXTENSION_NOT_PRESENT;
  const bool changed = excl_.NoteAcquire(int(result));
  if (excl_.state() == excl::State::Lost) stale_ = true;  // F2: a refused acquire rebuilds on the normal path at the next Acquire
  if (changed && excl_logged_) LogExclusive();  // a later rebuild: the first one is logged by Create()
#endif
}

void Swapchain::ReleaseExclusive(VkSwapchainKHR swapchain) {
#if defined(_WIN32)
  if (!swapchain || !excl_.ShouldRelease()) return;
  excl_.NoteReleased();
  if (const auto release = reinterpret_cast<ExclusiveFn>(release_fn_)) release(device_->device(), swapchain);  // (a result other than success changes nothing we can act on: the swapchain is going)
#else
  (void)swapchain;
#endif
}

// The swapchain's pass: the whole panel cleared (CLEAR), or S102 G1 lever 2 (DONT_CARE; compatible with the cleared one, so framebuffers and pipelines are
// shared; the caller clears the bars itself, present_clear.h). The no-clear pass is made only when asked (render_pass_noclear()).
VkRenderPass Swapchain::MakePass(VkAttachmentLoadOp load, VkImageLayout final_layout) {
  VkAttachmentDescription attachment{};
  attachment.format = format_;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = load;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = final_layout;  // S102 G2: TRANSFER_SRC for the passes that draw into a target of ours (capped, offscreen): never PRESENT_SRC on an image that is not presentable
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
  VkSubpassDependency dependencies[2] = {dependency, {}};
  VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass_info.attachmentCount = 1;
  pass_info.pAttachments = &attachment;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  pass_info.dependencyCount = 1;
  pass_info.pDependencies = dependencies;
  if (final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {  // S102 G2: the capped picture is read by the blit (and by ReadLast) right after the pass
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pass_info.dependencyCount = 2;
  }
  VkRenderPass pass = VK_NULL_HANDLE;
  vkCreateRenderPass(device_->device(), &pass_info, nullptr, &pass);
  return pass;
}

VkRenderPass Swapchain::render_pass() {
  if (offscreen_ || !capped_) return render_pass_;
  if (!render_pass_xfer_) render_pass_xfer_ = MakePass(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  return render_pass_xfer_;
}

VkRenderPass Swapchain::render_pass_noclear() {
  if (capped_ || offscreen_) {
    if (!render_pass_xfer_noclear_) render_pass_xfer_noclear_ = MakePass(VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return render_pass_xfer_noclear_;
  }
  if (!render_pass_noclear_) render_pass_noclear_ = MakePass(VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  return render_pass_noclear_;
}

// ---- S102 G2 ----
VkDeviceMemory Swapchain::BindNew(VkImage image) {
  VkMemoryRequirements need{};
  vkGetImageMemoryRequirements(device_->device(), image, &need);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = need.size;
  alloc.memoryTypeIndex = uint32_t(std::max(0, device_->FindMemoryType(need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)));
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (vkAllocateMemory(device_->device(), &alloc, nullptr, &memory) != VK_SUCCESS) return VK_NULL_HANDLE;
  vkBindImageMemory(device_->device(), image, memory, 0);
  return memory;
}

bool Swapchain::CreateOffscreen(Device* device, uint32_t w, uint32_t h, std::string& error) {
  device_ = device;
  offscreen_ = true;
  format_ = VK_FORMAT_B8G8R8A8_UNORM;  // the window's usual plain format
  render_pass_ = MakePass(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);  // (no swapchain extension here: never PRESENT_SRC)
  extent_ = {w, h};
  draw_extent_ = extent_;
  blit_ok_ = device_->FormatHas(format_, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
  VkDevice dev = device_->device();
  const uint32_t count = 3;
  images_.resize(count);
  views_.resize(count);
  framebuffers_.resize(count);
  finished_.resize(count);
  own_memory_.assign(count, VK_NULL_HANDLE);
  VkRenderPass pass = render_pass();
  for (uint32_t i = 0; i < count; ++i) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format_;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &info, nullptr, &images_[i]) != VK_SUCCESS || !(own_memory_[i] = BindNew(images_[i]))) {
      error = "Could not make the offscreen output images.";
      return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = images_[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &view, nullptr, &views_[i]);
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &views_[i];
    framebuffer.width = w;
    framebuffer.height = h;
    framebuffer.layers = 1;
    vkCreateFramebuffer(dev, &framebuffer, nullptr, &framebuffers_[i]);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev, &semaphore, nullptr, &finished_[i]);
  }
  Log("[rdr] offscreen output %ux%u (no display: the present draws into images nothing shows)", w, h);
  return true;
}

void Swapchain::ReleaseCap() {
  VkDevice dev = device_->device();
  for (CapTarget& c : cap_) {
    if (c.framebuffer) vkDestroyFramebuffer(dev, c.framebuffer, nullptr);
    if (c.view) vkDestroyImageView(dev, c.view, nullptr);
    if (c.image) vkDestroyImage(dev, c.image, nullptr);
    if (c.memory) vkFreeMemory(dev, c.memory, nullptr);
  }
  cap_.clear();
  capped_ = false;
}

bool Swapchain::MakeCap(uint32_t w, uint32_t h) {
  VkDevice dev = device_->device();
  cap_.assign(images_.size(), CapTarget{});
  capped_ = true;  // (render_pass() picks the transfer-src pass while capped)
  VkRenderPass pass = render_pass();
  for (CapTarget& c : cap_) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format_;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &info, nullptr, &c.image) != VK_SUCCESS || !(c.memory = BindNew(c.image))) {
      ReleaseCap();
      return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = c.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &view, nullptr, &c.view);
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &c.view;
    framebuffer.width = w;
    framebuffer.height = h;
    framebuffer.layers = 1;
    vkCreateFramebuffer(dev, &framebuffer, nullptr, &c.framebuffer);
  }
  draw_extent_ = {w, h};
  return true;
}

void Swapchain::NoteOutputCap(uint32_t cap_h) {
  if (offscreen_ || !cap_h) return;
  if (!cap_wanted_ && (quarter_turns_ & 1) == 0 && extent_.height > cap_h) {  // F9: only a cap that can apply asks for the rebuild
    cap_wanted_ = true;
    if (!dst_usage_) stale_ = true;  // the next Acquire rebuilds with TRANSFER_DST
  }
}

bool Swapchain::ApplyOutputCap(uint32_t cap_h, bool plant) {
  const bool turned = (quarter_turns_ & 1) != 0;
  const bool can = cap_h && !plant && !turned && extent_.height > cap_h && (offscreen_ || dst_usage_) && blit_ok_;
  if (!can) {
    if (capped_) {
      vkDeviceWaitIdle(device_->device());
      ReleaseCap();
      draw_extent_ = extent_;
    }
    return false;
  }
  const uint32_t w = std::max(1u, uint32_t(uint64_t(extent_.width) * cap_h / extent_.height));
  if (!capped_ || draw_extent_.height != cap_h || draw_extent_.width != w) {
    if (failed_cap_h_ == cap_h && failed_panel_h_ == extent_.height) return false;  // F7: it failed at this cap and panel: not retried until either changes
    vkDeviceWaitIdle(device_->device());
    ReleaseCap();
    g_out_makecap_tries.fetch_add(1);
    if (g_out_makecap_fail_plant.load() || !MakeCap(w, cap_h)) {
      ReleaseCap();
      draw_extent_ = extent_;
      failed_cap_h_ = cap_h, failed_panel_h_ = extent_.height;
      Log("[rdr] output cap: could not make the %ux%u target; drawing at the panel size until the cap or the panel changes", w, cap_h);
      return false;
    }
    Log("[rdr] output cap: drawn at %ux%u, stretched over %ux%u by a bilinear blit", w, cap_h, extent_.width, extent_.height);
  }
  return true;
}

void Swapchain::RecordCapBlit(VkCommandBuffer cmd, uint32_t i) {
  if (!capped_) return;
  const VkImageLayout final_layout = offscreen_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  // (the capped picture's own hand-over, colour write -> transfer read, is the pass's external dependency: MakePass)
  VkImageMemoryBarrier pre{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  pre.srcQueueFamilyIndex = pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  pre.image = images_[i];
  pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &pre);
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {int32_t(draw_extent_.width), int32_t(draw_extent_.height), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[1] = g_out_blit_plant.load() ? VkOffset3D{int32_t(draw_extent_.width), int32_t(draw_extent_.height), 1}  // TEST ONLY: the corner only
                                               : VkOffset3D{int32_t(extent_.width), int32_t(extent_.height), 1};
  vkCmdBlitImage(cmd, cap_[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
  VkImageMemoryBarrier post{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  post.dstAccessMask = offscreen_ ? VK_ACCESS_TRANSFER_READ_BIT : 0;
  post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  post.newLayout = final_layout;
  post.srcQueueFamilyIndex = post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  post.image = images_[i];
  post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, offscreen_ ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &post);
}

bool Swapchain::ReadLast(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
  if (!offscreen_ || !drawn_offscreen_) return false;
  VkDevice dev = device_->device();
  vkDeviceWaitIdle(dev);
  w = extent_.width, h = extent_.height;
  const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkBufferCreateInfo binfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  binfo.size = bytes;
  binfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &binfo, nullptr, &buffer);
  VkMemoryRequirements need{};
  vkGetBufferMemoryRequirements(dev, buffer, &need);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = need.size;
  alloc.memoryTypeIndex = uint32_t(std::max(0, device_->FindMemoryType(need.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)));
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  if (vkAllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS) {
    vkDestroyBuffer(dev, buffer, nullptr);
    return false;
  }
  vkBindBufferMemory(dev, buffer, memory, 0);
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = device_->queue_family();
  vkCreateCommandPool(dev, &pool_info, nullptr, &pool);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo calloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  calloc.commandPool = pool;
  calloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  calloc.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &calloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(cmd, images_[last_offscreen_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(device_->queue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(device_->queue());
  vkMapMemory(dev, memory, 0, bytes, 0, &mapped);
  rgba.resize(size_t(bytes));
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  for (size_t p = 0; p < size_t(w) * h; ++p) {  // BGRA8 -> RGBA8
    rgba[p * 4 + 0] = src[p * 4 + 2], rgba[p * 4 + 1] = src[p * 4 + 1], rgba[p * 4 + 2] = src[p * 4 + 0], rgba[p * 4 + 3] = 255;
  }
  vkUnmapMemory(dev, memory);
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyBuffer(dev, buffer, nullptr);
  vkFreeMemory(dev, memory, nullptr);
  return true;
}

void Swapchain::ReleaseImages() {
  VkDevice dev = device_->device();
  ReleaseCap();  // S102 G2
  if (offscreen_) {
    for (VkImage image : images_) vkDestroyImage(dev, image, nullptr);
    for (VkDeviceMemory memory : own_memory_) if (memory) vkFreeMemory(dev, memory, nullptr);
    own_memory_.clear();
  }
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
  draw_extent_ = extent;
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
  dst_usage_ = false;
  if (cap_wanted_ && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT, dst_usage_ = true;  // S102 G2: the cap's blit
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
#if defined(_WIN32)
  // S102 G2b: the only place the exclusive structs exist. ChainStructs() is false with the switch off, with the extension missing, and after a loss: those runs build none.
  ExclusiveInfo exclusive_info{VkStructureType(1000255000), nullptr, 3 /* APPLICATION_CONTROLLED: we acquire and release, so the answer is ours to log */};
  ExclusiveWin32Info exclusive_win32{VkStructureType(1000255001), nullptr, nullptr};
  bool chained = false;
  if (excl_.ChainStructs()) {
    exclusive_win32.hmonitor = rdr_MonitorFromWindow(window_.window, 2 /* MONITOR_DEFAULTTONEAREST: never null for a live window */);
    exclusive_info.next = &exclusive_win32;
    info.pNext = &exclusive_info;
    chained = true;
  }
#endif
#if defined(_WIN32)
  ReleaseExclusive(old);  // S102 G2b F4 (corrected): access does not carry over through oldSwapchain; the old one is released while it is not yet retired, and the new one is acquired below
#endif
  VkResult created = vkCreateSwapchainKHR(dev, &info, nullptr, &swapchain_);
#if defined(_WIN32)
  if (created != VK_SUCCESS && chained) {  // F1: the driver refused the exclusive swapchain: lost, say so, and retry once on the normal path
    chained = false;
    if (excl_.NoteCreateFailed() && excl_logged_) LogExclusive();
    info.pNext = nullptr;
    info.oldSwapchain = VK_NULL_HANDLE;  // the failed call retired the old one; it is destroyed below
    swapchain_ = VK_NULL_HANDLE;
    created = vkCreateSwapchainKHR(dev, &info, nullptr, &swapchain_);
  }
#endif
  if (created != VK_SUCCESS) {
    Problem("swapchain_create_failed", "%ux%u", extent.width, extent.height);
    swapchain_ = VK_NULL_HANDLE;
    if (old) vkDestroySwapchainKHR(dev, old, nullptr);
    return false;
  }
#if defined(_WIN32)
  if (excl_.AcquireAfterCreate(chained)) AcquireExclusive();  // S102 G2b F4: after EVERY create with the structs chained, a rebuild included
#endif
  blit_ok_ = device_->FormatHas(format_, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);  // S102 G2 F8
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
  {  // S102 R25-C2 (R25-C section 0 point 2): the pacing facts of the present path on one line (the swapchain line itself is parsed by the case zip, so it keeps its shape)
    int refresh_hz = 0;
#if defined(_WIN32)
    if (window_.window) {  // the panel's current refresh (GetDeviceCaps VREFRESH = 116), whole Hz; 0 = unknown
      if (void* dc = rdr_GetDC(window_.window)) {
        const int rate = rdr_GetDeviceCaps(dc, 116);
        rdr_ReleaseDC(window_.window, dc);
        refresh_hz = rate > 1 ? rate : 0;
      }
    }
#endif
    Log("[rdr] swapchain pacing: present mode %s (%d), %u images, display timing %s, display refresh %d Hz (0 = unknown)",
        present_mode_ == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : present_mode_ == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : present_mode_ == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "other",
        int(present_mode_), count, device_->caps().display_timing ? "yes" : "no", refresh_hz);
  }
  return true;
}

bool Swapchain::Acquire(VkSemaphore available, uint32_t& image_index) {
  if (offscreen_) {  // S102 G2: nothing to wait for (the submit does not wait on `available` either)
    image_index = last_offscreen_ = next_offscreen_++ % uint32_t(images_.size());
    drawn_offscreen_ = true;
    return true;
  }
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
  if (excl_.NeedsRebuild(int(result))) {  // S102 G2b: exclusivity lost (alt-tab ...): the next Acquire rebuilds on the normal path, no exclusive structs; every such result rebuilds, the line is the transition's
    stale_ = true;
    if (excl_.NoteResult(int(result))) LogExclusive();
    return false;
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    stale_ = true;
    return false;
  }
  if (result == VK_SUBOPTIMAL_KHR) stale_ = true;  // this image is still fine to use; rebuild for the next one
  else if (result != VK_SUCCESS) {
    if (result == VK_ERROR_DEVICE_LOST) lost_at_ = "vkAcquireNextImageKHR";  // S102 D1: the renderer names it once (OnDeviceLost)
    Problem("swapchain_acquire_failed", "VkResult %d", int(result));
    return false;
  }
  return true;
}

bool Swapchain::Present(uint32_t image_index, VkSemaphore finished, uint64_t desired_ns) {
  if (offscreen_) return !DeviceLost();  // S102 D1: no present is accepted once the device is lost
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
  else if (excl_.NeedsRebuild(int(result))) {  // S102 G2b: exclusivity lost at the present: rebuild on the normal path, one line on the transition, never a crash
    stale_ = true;
    if (excl_.NoteResult(int(result))) LogExclusive();
  } else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) stale_ = true;
  else if (result != VK_SUCCESS) {
    Problem("swapchain_present_failed", "VkResult %d", int(result));
    if (result == VK_ERROR_DEVICE_LOST) lost_at_ = "vkQueuePresentKHR";  // S102 D1
  }
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
  // S102 D1: after a lost device vkQueuePresentKHR still returned VK_SUCCESS on the Surface (Intel UHD 620): the GPU drew nothing, the picture froze and
  // every frame counted as presented (benches read ~60 fps). No present is accepted once the device is lost.
  return (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && !DeviceLost();  // lane 256: accepted, as the SDK presenter counts it
}

void Swapchain::Destroy() {
  if (!device_) return;
  VkDevice dev = device_->device();
  ReleaseExclusive(swapchain_);  // S102 G2b
  ReleaseImages();
  if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
  if (render_pass_) vkDestroyRenderPass(dev, render_pass_, nullptr);
  if (render_pass_noclear_) vkDestroyRenderPass(dev, render_pass_noclear_, nullptr);  // S102 G1
  if (render_pass_xfer_) vkDestroyRenderPass(dev, render_pass_xfer_, nullptr);  // S102 G2
  if (render_pass_xfer_noclear_) vkDestroyRenderPass(dev, render_pass_xfer_noclear_, nullptr);
  render_pass_xfer_ = render_pass_xfer_noclear_ = VK_NULL_HANDLE;
  if (surface_) vkDestroySurfaceKHR(device_->instance(), surface_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  render_pass_ = VK_NULL_HANDLE;
  render_pass_noclear_ = VK_NULL_HANDLE;
  surface_ = VK_NULL_HANDLE;
}

}  // namespace rdr
