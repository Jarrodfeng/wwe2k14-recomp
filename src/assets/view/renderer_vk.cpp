// The Vulkan backend (renderer_vk.h). Session 98 Arena Viewer phone port, stage B. Every pass mirrors renderer_d3d11.cpp (same order,
// same states, same constants from the shared Renderer); the comments there carry the evidence, not repeated here.
#include "renderer_vk.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "spirv/viewer_spv.h"

using namespace vkfn;
using assets::TexFormat;

namespace view {

std::unique_ptr<Renderer> MakeRendererVk() { return std::make_unique<RendererVk>(); }

namespace {

constexpr VkFormat kSceneFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kOutFmt = VK_FORMAT_B8G8R8A8_UNORM;  // shots and the window's picture (D3D11: B8G8R8A8_UNORM)

struct Buf {
  VkBuffer b = VK_NULL_HANDLE;
  VkDeviceMemory m = VK_NULL_HANDLE;
  void* map = nullptr;
  VkDeviceSize size = 0;
  bool coherent = true;
};
struct BufRef { VkBuffer b = VK_NULL_HANDLE; VkDeviceSize off = 0, size = 0; };

struct Img {
  VkImage i = VK_NULL_HANDLE;
  VkImageView v = VK_NULL_HANDLE;
  VkDeviceMemory m = VK_NULL_HANDLE;  // own memory (null when it lives in a model's block)
  VkFormat f = VK_FORMAT_UNDEFINED;
  int w = 0, h = 0;
  uint32_t mips = 1;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  bool ok() const { return i != VK_NULL_HANDLE; }
};

// one block of device memory a model's textures are placed in (freed with the model)
struct Block { VkDeviceMemory m = VK_NULL_HANDLE; uint32_t type = 0; VkDeviceSize size = 0, used = 0; };

// ---- BC1-3 -> RGBA8 (devices without textureCompressionBC: the phone may lack it) -----------------------------------------
void Rgb565(uint16_t c, uint8_t o[3]) {
  o[0] = uint8_t(((c >> 11) & 31) * 255 / 31), o[1] = uint8_t(((c >> 5) & 63) * 255 / 63), o[2] = uint8_t((c & 31) * 255 / 31);
}
void DecodeBlock(TexFormat f, const uint8_t* b, uint8_t out[16][4]) {
  const uint8_t* cb = f == TexFormat::kBC1 ? b : b + 8;
  const uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
  uint8_t pal[4][4];
  Rgb565(c0, pal[0]), Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (f != TexFormat::kBC1 || c0 > c1) {
    for (int k = 0; k < 3; k++) pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3), pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; k++) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
    pal[2][3] = 255, pal[3][3] = 0;
  }
  const uint32_t idx = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
  for (int p = 0; p < 16; p++) memcpy(out[p], pal[(idx >> (2 * p)) & 3], 4);
  if (f == TexFormat::kBC2) {
    for (int p = 0; p < 16; p++) out[p][3] = uint8_t(((b[p / 2] >> (4 * (p & 1))) & 15) * 17);
  } else if (f == TexFormat::kBC3) {
    const int a0 = b[0], a1 = b[1];
    int ap[8] = {a0, a1};
    if (a0 > a1) for (int k = 1; k < 7; k++) ap[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    else { for (int k = 1; k < 5; k++) ap[k + 1] = ((5 - k) * a0 + k * a1) / 5; ap[6] = 0, ap[7] = 255; }
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++) bits |= uint64_t(b[2 + k]) << (8 * k);
    for (int p = 0; p < 16; p++) out[p][3] = uint8_t(ap[(bits >> (3 * p)) & 7]);
  }
}
void DecodeBC(TexFormat f, const uint8_t* src, uint32_t w, uint32_t h, uint8_t* dst) {
  const size_t bs = f == TexFormat::kBC1 ? 8 : 16;
  for (uint32_t by = 0; by < (h + 3) / 4; by++)
    for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
      uint8_t px[16][4];
      DecodeBlock(f, src + (by * ((w + 3) / 4) + bx) * bs, px);
      for (int p = 0; p < 16; p++) {
        const uint32_t x = bx * 4 + p % 4, y = by * 4 + p / 4;
        if (x < w && y < h) memcpy(dst + (size_t(y) * w + x) * 4, px[p], 4);
      }
    }
}

size_t MipBytes(TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case TexFormat::kBC1: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8;
    case TexFormat::kBC2:
    case TexFormat::kBC3: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
    case TexFormat::kB4G4R4A4: return (size_t)w * h * 2;
    case TexFormat::kB8G8R8A8:
    case TexFormat::kR8G8B8A8: return (size_t)w * h * 4;
    default: return 0;
  }
}

std::string VkStr(const char* what, VkResult r) {
  char b[128];
  snprintf(b, sizeof b, "%s failed (VkResult %d)", what, (int)r);
  return b;
}

// The model's Vulkan objects (see AddModel's hooks).
struct VkModel;

// Programs (pipelines are keyed by program + state + formats)
enum Prog : uint8_t { kPScene, kPCaster, kPRim, kPPlane, kPId, kPMask, kPSprite, kPFlash, kPBlurDown, kPBlur, kPRim13, kPPost0, kPComp = kPPost0 + 7, kPCount };

struct PipeKey {
  uint8_t prog = 0, cull = 0, blend = 0, depth = 0;  // depth: 0 off, 1 test + write, 2 test only
  VkFormat color = VK_FORMAT_UNDEFINED, depthf = VK_FORMAT_UNDEFINED;
  bool operator==(const PipeKey& o) const { return !memcmp(this, &o, sizeof *this); }
};
struct PipeKeyHash {
  size_t operator()(const PipeKey& k) const { return size_t(k.prog) | size_t(k.cull) << 8 | size_t(k.blend) << 12 | size_t(k.depth) << 16 | (size_t(k.color) << 20 ^ size_t(k.depthf) << 40); }
};

}  // namespace

struct RendererVk::Impl {
  RendererVk* r;
  explicit Impl(RendererVk* rr) : r(rr) {}

  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  uint32_t qfam = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties memprops{};
  bool bc_ok = false, aniso_ok = false;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swap = VK_NULL_HANDLE;
  VkFormat swap_fmt = VK_FORMAT_UNDEFINED;
  std::vector<VkImage> swap_imgs;
  std::vector<VkSemaphore> sem_render;
  VkSemaphore sem_acquire = VK_NULL_HANDLE;
  bool windowed = false;
  // Session 98 stage C review: the surface is away on purpose (Android background, SurfaceGone) - else a missing surface is retried
  bool surface_away = false;
  int surface_retry = 0;

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE, up = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool recording = false, up_open = false;
  uint64_t frame_id = 0;

  VkSampler s_aniso_wrap = VK_NULL_HANDLE, s_aniso_clamp = VK_NULL_HANDLE, s_lin = VK_NULL_HANDLE, s_point = VK_NULL_HANDLE;
  VkDescriptorSetLayout l_scene0 = VK_NULL_HANDLE, l_scene1 = VK_NULL_HANDLE, l_small_lin = VK_NULL_HANDLE, l_small_aniso = VK_NULL_HANDLE;
  VkPipelineLayout pl_scene = VK_NULL_HANDLE, pl_small_lin = VK_NULL_HANDLE, pl_small_aniso = VK_NULL_HANDLE;
  std::map<std::string, VkShaderModule> mods;
  std::unordered_map<PipeKey, VkPipeline, PipeKeyHash> pipes;
  std::map<uint64_t, VkRenderPass> rps;
  std::vector<VkFramebuffer> fb_dead;  // this frame's framebuffers (destroyed at the next Begin)
  std::vector<VkDescriptorPool> dpools;
  size_t dpool_cur = 0;

  // ring: per-frame constants, bone palettes, sprite vertices
  std::vector<Buf> ring;
  size_t ring_cur = 0;
  VkDeviceSize ring_off = 0;
  static constexpr VkDeviceSize kRingChunk = 8u << 20;

  Buf stage;   // upload staging (host visible)
  VkDeviceSize stage_used = 0;
  Buf readback;  // shots / picks
  Buf ident;     // [0, 64): identity instance; [64, 128): identity bone
  Img zero;      // 1x1 (0, 0, 0, 0): what an unbound D3D11 slot reads
  Img props_rt, props_depth, wr_depth, plane_a, plane_b, plane_c, rim_a, rim_b, rim_depth;
  struct PostSet {
    Img scene, depth, near_a, near_b, far_a, far_b, bright, bl_a, bl_b, glare;
    int w = 0, h = 0;
    unsigned stamp = 0;
  };
  PostSet sets[2];
  PostSet* s = &sets[0];
  unsigned tick = 0;
  Img live, win_out, shot_out, id_img, id_depth, mask;
  bool live_has = false;
  std::vector<Img> sprite_tex;
  std::map<VkImageView, Img> ui_imgs;
  std::function<uint64_t(VkImageView)> ui_add;
  std::function<void(uint64_t)> ui_remove;
  bool in_overlay = false;
  std::function<void(uint32_t)> on_swap;            // the UI backend's: a swap chain with a new minimum image count
  uint32_t swap_min = 0;
  std::function<bool(void*, uint64_t*)> make_surface;  // the window's (Surface::vk_create_surface): a new VkSurfaceKHR after a loss

  // GPU timer
  VkQueryPool qpool = VK_NULL_HANDLE;
  uint32_t ts_bits = 0;
  std::vector<std::string> marks;
  bool timer_on = false, timer_ok = false;
  std::vector<std::pair<std::string, double>> last;
  double last_total = 0;

  // pick readbacks (3 slots of 8 bytes in `pick_buf`)
  Buf pick_buf;
  bool pending[3] = {};
  unsigned pend_seq[3] = {}, seq = 0;
  uint32_t pend_gen[3] = {};
  int draw_inst = -1;
  BufRef cur_pick;     // PickCB of the draws now (zero outside the ID / mask passes)
  BufRef cur_set0_frame;
  VkPipeline bound_pipe = VK_NULL_HANDLE;

  // ---- basics ----
  bool Check(VkResult res) {
    if (res == VK_ERROR_DEVICE_LOST) r->lost_ = true;
    return res == VK_SUCCESS;
  }
  int MemType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
  }
  bool AllocMem(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, VkDeviceMemory& m, uint32_t* type_out = nullptr) {
    int t = MemType(req.memoryTypeBits, want);
    if (t < 0 && (want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) t = MemType(req.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) return false;
    if (type_out) *type_out = (uint32_t)t;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = (uint32_t)t;
    return Check(vkAllocateMemory(dev, &ai, nullptr, &m));
  }
  // host: mapped, coherent (uploads, the ring); readback: mapped, HOST_CACHED when the GPU has it (CPU reads), maybe not coherent.
  bool MakeBuf(Buf& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool readback = false) {
    b = Buf();
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = std::max<VkDeviceSize>(size, 16), ci.usage = usage, ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vkCreateBuffer(dev, &ci, nullptr, &b.b))) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.b, &req);
    const VkMemoryPropertyFlags want = readback ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                                       : host   ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (!AllocMem(req, want, b.m, &type)) {
      vkDestroyBuffer(dev, b.b, nullptr), b.b = VK_NULL_HANDLE;
      return false;
    }
    vkBindBufferMemory(dev, b.b, b.m, 0);
    if (host || readback) vkMapMemory(dev, b.m, 0, VK_WHOLE_SIZE, 0, &b.map);
    b.coherent = (memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    b.size = ci.size;
    return true;
  }
  // Before the CPU reads what the GPU wrote: the copies' writes made visible to the host (recorded before the submit)...
  void HostReadBarrier(VkCommandBuffer c) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
  }
  // ... and, after the wait, the mapping refreshed when the memory is not coherent.
  void Invalidate(Buf& b) {
    if (b.coherent || !b.m) return;
    VkMappedMemoryRange r0{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r0.memory = b.m, r0.offset = 0, r0.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(dev, 1, &r0);
  }
  void FreeBuf(Buf& b) {
    if (b.b) vkDestroyBuffer(dev, b.b, nullptr);
    if (b.m) vkFreeMemory(dev, b.m, nullptr);
    b = Buf();
  }
  // An image; `blocks` given = placed in the model's blocks (else its own memory).
  bool MakeImg(Img& im, VkFormat f, int w, int h, uint32_t mips, VkImageUsageFlags usage, std::vector<Block>* blocks = nullptr) {
    im = Img();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = f, ci.extent = {(uint32_t)std::max(w, 1), (uint32_t)std::max(h, 1), 1};
    ci.mipLevels = mips, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage, ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE, ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vkCreateImage(dev, &ci, nullptr, &im.i))) { im.i = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, im.i, &req);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    if (blocks) {
      const int t = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      Block* bk = nullptr;
      for (Block& b : *blocks) {
        const VkDeviceSize o = (b.used + req.alignment - 1) / req.alignment * req.alignment;
        if ((int)b.type == t && o + req.size <= b.size) { bk = &b, off = o; break; }
      }
      if (!bk && t >= 0) {
        Block nb;
        nb.type = (uint32_t)t, nb.size = std::max<VkDeviceSize>(req.size, 32u << 20);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = nb.size, ai.memoryTypeIndex = nb.type;
        if (Check(vkAllocateMemory(dev, &ai, nullptr, &nb.m))) blocks->push_back(nb), bk = &blocks->back(), off = 0;
      }
      if (bk) mem = bk->m, bk->used = off + req.size;
    } else if (AllocMem(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem)) {
      im.m = mem;
    }
    if (!mem) { vkDestroyImage(dev, im.i, nullptr); im = Img(); return false; }
    vkBindImageMemory(dev, im.i, mem, off);
    const bool depth = f == VK_FORMAT_D32_SFLOAT;
    im.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.i, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = f;
    vi.subresourceRange = {im.aspect, 0, mips, 0, 1};
    if (!Check(vkCreateImageView(dev, &vi, nullptr, &im.v))) { FreeImg(im); return false; }
    im.f = f, im.w = std::max(w, 1), im.h = std::max(h, 1), im.mips = mips;
    return true;
  }
  void FreeImg(Img& im) {
    if (im.v) vkDestroyImageView(dev, im.v, nullptr);
    if (im.i) vkDestroyImage(dev, im.i, nullptr);
    if (im.m) vkFreeMemory(dev, im.m, nullptr);
    im = Img();
  }
  static VkImageLayout ReadLayout(const Img& im) {
    return im.aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  // Conservative: every barrier waits for all earlier work (the viewer is not bound by this; simple and correct first).
  void Barrier(VkCommandBuffer c, Img& im, VkImageLayout to) {
    if (im.layout == to || !im.i) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im.layout, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.i, b.subresourceRange = {im.aspect, 0, im.mips, 0, 1};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    im.layout = to;
  }
  void MemBarrier(VkCommandBuffer c) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
  }
  void ClearImg(VkCommandBuffer c, Img& im, float r0, float g0, float b0, float a0) {
    Barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue cv{};
    cv.float32[0] = r0, cv.float32[1] = g0, cv.float32[2] = b0, cv.float32[3] = a0;
    VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, im.mips, 0, 1};
    vkCmdClearColorImage(c, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
  }

  // ---- command buffers ----
  void BeginCmd() {
    for (VkFramebuffer f : fb_dead) vkDestroyFramebuffer(dev, f, nullptr);
    fb_dead.clear();
    for (VkDescriptorPool p : dpools) vkResetDescriptorPool(dev, p, 0);
    dpool_cur = 0;
    ring_cur = 0, ring_off = 0;
    frame_id++;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    recording = true;
    bound_pipe = VK_NULL_HANDLE;
    PickCB zero_pick{};
    cur_pick = Push(&zero_pick, sizeof zero_pick);
  }
  // Ends, submits and waits. wait_sem / signal_sem: the swap chain's (the present).
  bool SubmitWait(VkSemaphore wait_sem = VK_NULL_HANDLE, VkSemaphore signal_sem = VK_NULL_HANDLE) {
    if (!recording) return false;
    recording = false;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    si.commandBufferCount = 1, si.pCommandBuffers = &cmd;
    if (wait_sem) si.waitSemaphoreCount = 1, si.pWaitSemaphores = &wait_sem, si.pWaitDstStageMask = &ws;
    if (signal_sem) si.signalSemaphoreCount = 1, si.pSignalSemaphores = &signal_sem;
    if (!Check(vkQueueSubmit(queue, 1, &si, fence))) return false;
    const bool ok = Check(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkResetFences(dev, 1, &fence);
    if (ok && timer_on) CollectTimer();
    return ok;
  }
  // Uploads go into their own command buffer (`up`), flushed (submitted + waited) before anything uses them.
  void UpBegin() {
    if (up_open) return;
    vkResetCommandBuffer(up, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(up, &bi);
    up_open = true;
    stage_used = 0;
  }
  bool UpFlush() {
    if (!up_open) return true;
    up_open = false;
    vkEndCommandBuffer(up);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &up;
    VkFence f = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fi, nullptr, &f);
    bool ok = Check(vkQueueSubmit(queue, 1, &si, f)) && Check(vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
    vkDestroyFence(dev, f, nullptr);
    stage_used = 0;
    return ok;
  }
  // Room for `bytes` in the staging buffer (flushes / grows it); returns the offset.
  VkDeviceSize StageAlloc(VkDeviceSize bytes) {
    bytes = (bytes + 15) & ~VkDeviceSize(15);
    if (stage_used + bytes > stage.size) {
      if (up_open) UpFlush(), UpBegin();
      if (bytes > stage.size) {
        FreeBuf(stage);
        MakeBuf(stage, std::max<VkDeviceSize>(bytes, 64u << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
      }
    }
    const VkDeviceSize o = stage_used;
    stage_used += bytes;
    return o;
  }

  // ---- ring + descriptor sets ----
  BufRef Push(const void* data, size_t bytes, VkDeviceSize align = 256) {
    for (;;) {
      if (ring_cur >= ring.size()) {
        Buf b;
        MakeBuf(b, std::max<VkDeviceSize>(kRingChunk, bytes), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        ring.push_back(b);
      }
      Buf& b = ring[ring_cur];
      const VkDeviceSize o = (ring_off + align - 1) / align * align;
      if (o + bytes <= b.size) {
        memcpy((uint8_t*)b.map + o, data, bytes);
        ring_off = o + bytes;
        return {b.b, o, (VkDeviceSize)bytes};
      }
      ring_cur++, ring_off = 0;
    }
  }
  VkDescriptorSet AllocSet(VkDescriptorSetLayout l) {
    for (;;) {
      if (dpool_cur >= dpools.size()) {
        const VkDescriptorPoolSize sz[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 49152},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_SAMPLER, 8192}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 8192, ci.poolSizeCount = 4, ci.pPoolSizes = sz;
        VkDescriptorPool p = VK_NULL_HANDLE;
        if (!Check(vkCreateDescriptorPool(dev, &ci, nullptr, &p))) return VK_NULL_HANDLE;
        dpools.push_back(p);
      }
      VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      ai.descriptorPool = dpools[dpool_cur], ai.descriptorSetCount = 1, ai.pSetLayouts = &l;
      VkDescriptorSet ds = VK_NULL_HANDLE;
      if (vkAllocateDescriptorSets(dev, &ai, &ds) == VK_SUCCESS) return ds;
      dpool_cur++;
    }
  }
  // A batch of descriptor writes for one set.
  struct Writes {
    VkDescriptorSet set;
    VkWriteDescriptorSet w[16];
    VkDescriptorBufferInfo bi[16];
    VkDescriptorImageInfo ii[16];
    uint32_t n = 0;
    void Buffer(uint32_t binding, VkDescriptorType t, const BufRef& b) {
      bi[n] = {b.b, b.off, b.size};
      w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[n].dstSet = set, w[n].dstBinding = binding, w[n].descriptorCount = 1, w[n].descriptorType = t, w[n].pBufferInfo = &bi[n];
      n++;
    }
    void Image(uint32_t binding, VkImageView v, VkImageLayout l) {
      ii[n] = {VK_NULL_HANDLE, v, l};
      w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[n].dstSet = set, w[n].dstBinding = binding, w[n].descriptorCount = 1, w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w[n].pImageInfo = &ii[n];
      n++;
    }
  };
  void Commit(Writes& w) { vkUpdateDescriptorSets(dev, w.n, w.w, 0, nullptr); }
  void ImgW(Writes& w, uint32_t binding, const Img& im) { w.Image(binding, im.v, ReadLayout(im)); }

  // ---- render passes / pipelines ----
  VkRenderPass RenderPass(VkFormat color, VkFormat depth, VkAttachmentLoadOp cl, VkAttachmentLoadOp dl) {
    const uint64_t key = uint64_t(color) | uint64_t(depth) << 24 | uint64_t(cl) << 48 | uint64_t(dl) << 56;
    auto it = rps.find(key);
    if (it != rps.end()) return it->second;
    VkAttachmentDescription at[2]{};
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    uint32_t n = 0;
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (color != VK_FORMAT_UNDEFINED) {
      at[n].format = color, at[n].samples = VK_SAMPLE_COUNT_1_BIT, at[n].loadOp = cl, at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      at[n].initialLayout = at[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      cref.attachment = n++;
      sp.colorAttachmentCount = 1, sp.pColorAttachments = &cref;
    }
    if (depth != VK_FORMAT_UNDEFINED) {
      at[n].format = depth, at[n].samples = VK_SAMPLE_COUNT_1_BIT, at[n].loadOp = dl, at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      at[n].initialLayout = at[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      dref.attachment = n++;
      sp.pDepthStencilAttachment = &dref;
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n, ci.pAttachments = at, ci.subpassCount = 1, ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(dev, &ci, nullptr, &rp);
    rps[key] = rp;
    return rp;
  }
  // Begins a render pass on `color` / `depth` (either may be null), w x h = the attachment's, D3D's viewport (negative height).
  VkRenderPass BeginRP(Img* color, Img* depth, VkAttachmentLoadOp cl, VkAttachmentLoadOp dl, const float clear[4] = nullptr, float clear_depth = 1.f) {
    MemBarrier(cmd);
    if (color) Barrier(cmd, *color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) Barrier(cmd, *depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    const VkRenderPass rp = RenderPass(color ? color->f : VK_FORMAT_UNDEFINED, depth ? depth->f : VK_FORMAT_UNDEFINED, cl, dl);
    const int w = color ? color->w : depth->w, h = color ? color->h : depth->h;
    VkImageView views[2];
    uint32_t n = 0;
    if (color) views[n++] = color->v;
    if (depth) views[n++] = depth->v;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = rp, fi.attachmentCount = n, fi.pAttachments = views, fi.width = (uint32_t)w, fi.height = (uint32_t)h, fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(dev, &fi, nullptr, &fb);
    fb_dead.push_back(fb);
    VkClearValue cv[2]{};
    n = 0;
    if (color) { if (clear) memcpy(cv[n].color.float32, clear, 16); n++; }
    if (depth) cv[n++].depthStencil = {clear_depth, 0};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp, bi.framebuffer = fb, bi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}}, bi.clearValueCount = n, bi.pClearValues = cv;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0, (float)h, (float)w, -(float)h, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    bound_pipe = VK_NULL_HANDLE;
    return rp;
  }
  void EndRP() { vkCmdEndRenderPass(cmd); }

  VkShaderModule Mod(const char* src, const char* entry) {
    const std::string key = std::string(src) + ":" + entry;
    auto it = mods.find(key);
    if (it != mods.end()) return it->second;
    const spv::Entry* e = spv::Find(src, entry);
    if (!e) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = e->bytes, ci.pCode = e->code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    mods[key] = m;
    return m;
  }

  VkPipeline Pipe(const PipeKey& k) {
    auto it = pipes.find(k);
    if (it != pipes.end()) return it->second;
    const char *src = "scene", *vsn = "vs", *psn = "ps";
    VkPipelineLayout layout = pl_scene;
    int vin = 1;  // 0 none, 1 scene, 2 flash, 3 sprite
    switch (k.prog) {
      case kPScene: break;
      case kPCaster: psn = "ps_caster"; break;
      case kPRim: psn = "ps_rim"; break;
      case kPPlane: vsn = "vs_plane", psn = "ps_plane"; break;
      case kPId: vsn = "vs_id", psn = "ps_id"; break;
      case kPMask: vsn = "vs_mask", psn = "ps_mask"; break;
      case kPSprite: src = "sprite", vsn = "vs_sprite", psn = "ps_sprite", layout = pl_small_aniso, vin = 3; break;
      case kPFlash: src = "flash", vsn = "vs_flash", psn = "ps_flash", layout = pl_small_aniso, vin = 2; break;
      case kPBlurDown: src = "blur", vsn = "vsFull", psn = "psDown", layout = pl_small_lin, vin = 0; break;
      case kPBlur: src = "blur", vsn = "vsFull", psn = "psBlur", layout = pl_small_lin, vin = 0; break;
      case kPRim13: src = "blur", vsn = "vsFull", psn = "psRim13", layout = pl_small_lin, vin = 0; break;
      case kPComp: src = "comp", vsn = "vs_fs", psn = "ps_comp", layout = pl_small_lin, vin = 0; break;
      default:  // the post chain
        src = "post", vsn = "vsFull", psn = PostShaderEntry((PostShader)(k.prog - kPPost0)), layout = pl_small_lin, vin = 0;
        break;
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, Mod(src, vsn), vsn};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, Mod(src, psn), psn};
    // (DXC keeps the HLSL entry name as the SPIR-V entry point)
    VkVertexInputBindingDescription vb[2]{};
    VkVertexInputAttributeDescription va[12]{};
    uint32_t nb = 0, na = 0;
    if (vin == 1 || vin == 2) {
      vb[nb++] = {0, (uint32_t)sizeof(assets::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
      using V = assets::Vertex;
      if (vin == 1) {
        va[na++] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(V, pos)};
        va[na++] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(V, normal)};
        va[na++] = {2, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(V, uv0)};
        va[na++] = {3, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(V, uv1)};
        va[na++] = {4, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)offsetof(V, color)};
        va[na++] = {5, 0, VK_FORMAT_R16G16B16A16_UINT, (uint32_t)offsetof(V, bones)};  // (S99: 16-bit bone indices)
        va[na++] = {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(V, weights)};
        va[na++] = {7, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(V, tangent)};
        vb[nb++] = {1, 64, VK_VERTEX_INPUT_RATE_INSTANCE};
        for (uint32_t k2 = 0; k2 < 4; k2++) va[na++] = {8 + k2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * k2};
      } else {
        va[na++] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(V, pos)};
        va[na++] = {1, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(V, uv0)};
        va[na++] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)offsetof(V, color)};
      }
    } else if (vin == 3) {
      vb[nb++] = {0, 36, VK_VERTEX_INPUT_RATE_VERTEX};
      va[na++] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
      va[na++] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 12};
      va[na++] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb, vi.pVertexBindingDescriptions = vb, vi.vertexAttributeDescriptionCount = na, vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1, vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.lineWidth = 1.f;
    // D3D11's default winding (front = clockwise on screen); the negative-height viewport keeps D3D's screen orientation
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.cullMode = k.cull == 1 ? VK_CULL_MODE_BACK_BIT : k.cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    if (k.prog == kPMask) rs.depthBiasEnable = VK_TRUE, rs.depthBiasConstantFactor = -4.f, rs.depthBiasSlopeFactor = -2.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.depth ? VK_TRUE : VK_FALSE, ds.depthWriteEnable = k.depth == 1 ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    switch (k.blend) {
      case 1:  // alpha (D3D bs_blend_)
        cb.blendEnable = VK_TRUE, cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
      case 2:  // additive (bs_add_)
        cb.blendEnable = VK_TRUE, cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
      case 3:  // max (the plane shadow)
        cb.blendEnable = VK_TRUE, cb.srcColorBlendFactor = cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE, cb.colorBlendOp = VK_BLEND_OP_MAX;
        cb.srcAlphaBlendFactor = cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE, cb.alphaBlendOp = VK_BLEND_OP_MAX;
        break;
      case 4:  // the highlight composite (premultiplied, the back buffer's alpha stays)
        cb.blendEnable = VK_TRUE, cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE, cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        break;
      case 5: cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT; break;  // the highlight mask: R = selected
      case 6: cb.colorWriteMask = VK_COLOR_COMPONENT_G_BIT; break;  //                     G = hovered
      default: break;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = k.color != VK_FORMAT_UNDEFINED ? 1 : 0, cbs.pAttachments = &cb;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2, ci.pStages = st, ci.pVertexInputState = &vi, ci.pInputAssemblyState = &ia, ci.pViewportState = &vp;
    ci.pRasterizationState = &rs, ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds, ci.pColorBlendState = &cbs, ci.pDynamicState = &dy;
    ci.layout = layout;
    ci.renderPass = RenderPass(k.color, k.depthf, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD);
    VkPipeline p = VK_NULL_HANDLE;
    if (!Check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p))) fprintf(stderr, "vk: pipeline %d failed\n", (int)k.prog);
    pipes[k] = p;
    return p;
  }
  void BindPipe(VkPipeline p) {
    if (p != bound_pipe) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p), bound_pipe = p;
  }

  // ---- init ----
  bool Init(const Surface& surf, int w, int h, std::string& err);
  bool MakeSwapchain(int w, int h, std::string& err);
  void DestroySwapchain();
  bool SurfaceSizeChanged();
  void RecreateSwap(int w, int h);
  void DropSurface();
  bool NewSurface();
  void ResetAcquire();
  bool EnsurePost(int w, int h);
  bool EnsureImg(Img& im, VkFormat f, int w, int h, VkImageUsageFlags usage) {
    if (im.ok() && im.w == w && im.h == h && im.f == f) return true;
    if (im.ok()) { vkDeviceWaitIdle(dev); FreeImg(im); }
    return MakeImg(im, f, w, h, 1, usage);
  }
  bool UploadImage(Img& im, const assets::Texture& t, std::vector<Block>* blocks, std::vector<std::string>& warnings);
  void CollectTimer();
  void Mark(const char* name) {
    if (!timer_on || !qpool || marks.size() + 1 >= 64) return;
    marks.push_back(name);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, (uint32_t)marks.size());
  }

  // ---- the frame (renderer_d3d11.cpp Frame and its passes) ----
  VkDescriptorSet SceneSet0(const FrameCB& fcb, bool maps);
  void DrawMesh(const GpuModel& gm, const GpuMesh& me, bool blended);
  void ShadowPasses(const Camera& cam, int w, int h);
  void Blur(Img& src, Img& dst, Prog prog, float dx, float dy);
  void Scene(int w, int h, const Camera& cam);
  void SpritePass(const FrameCB& fcb);
  void IdPass(const Camera& cam, int w, int h, int sx, int sy);
  void QueueReadback(int x, int y);
  void PollReadback();
  void Post(Img& out, const std::vector<PostStep>& plan);
  void FlashPass(Img& out, const FrameCB& fcb);
  void HighlightPass(Img& out, int w, int h, const FrameCB& fcb);
  void Frame(Img& out, int w, int h, const Camera& cam, bool window);
  bool ReadImage(Img& im, std::vector<uint8_t>& px);  // (recording) copy into `readback`, submit, wait, read
};

// The model's objects: textures (placed in the model's own memory blocks), one device buffer with every mesh's vertices / indices and
// every placement's instance rows, the bone palette (CPU; copied into the ring each frame).
namespace {
struct VkModel : GpuModelData {
  RendererVk::Impl* im = nullptr;
  std::vector<Img> tex;
  std::vector<uint64_t> ui_ids;
  std::vector<Block> blocks;
  std::vector<uint8_t> cpu;  // geometry until FinishModel
  struct Range { VkDeviceSize vb = 0, ib = 0; };
  std::vector<Range> meshes;
  std::vector<VkDeviceSize> placements;
  Buf geo;
  std::vector<assets::Matrix> bones;
  uint64_t bones_frame = ~0ull;
  BufRef bones_ref;
  ~VkModel() override;
  VkDeviceSize Append(const void* p, size_t n) {
    const VkDeviceSize o = (cpu.size() + 15) & ~size_t(15);
    cpu.resize(o + n);
    memcpy(cpu.data() + o, p, n);
    return o;
  }
};
VkModel& VM(const GpuModel& g) { return *static_cast<VkModel*>(g.gpu.get()); }
}  // namespace

VkModel::~VkModel() {
  if (!im || !im->dev) return;
  for (uint64_t id : ui_ids) if (id && im->ui_remove) im->ui_remove(id);
  for (Img& t : tex) im->FreeImg(t);
  im->FreeBuf(geo);
  for (Block& b : blocks) vkFreeMemory(im->dev, b.m, nullptr);
}

// ---- init ------------------------------------------------------------------------------------------------------------

bool RendererVk::Impl::Init(const Surface& surf, int w, int h, std::string& err) {
  if (!LoadLoader(err)) return false;
  windowed = (bool)surf.vk_create_surface;
  std::vector<const char*> exts;
  for (const std::string& e : surf.vk_instance_exts) exts.push_back(e.c_str());
  const char* val = getenv("WWE_VK_VALIDATION");
  std::vector<const char*> layers;
  if (val && *val == '1') layers.push_back("VK_LAYER_KHRONOS_validation");
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "wwe2k14 asset viewer", ai.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &ai, ici.enabledExtensionCount = (uint32_t)exts.size(), ici.ppEnabledExtensionNames = exts.data();
  ici.enabledLayerCount = (uint32_t)layers.size(), ici.ppEnabledLayerNames = layers.data();
  VkResult res = vkCreateInstance(&ici, nullptr, &inst);
  if (res != VK_SUCCESS && !layers.empty()) { ici.enabledLayerCount = 0; res = vkCreateInstance(&ici, nullptr, &inst); }
  if (res != VK_SUCCESS) { err = VkStr("vkCreateInstance", res); return false; }
  LoadInstance(inst);
  if (windowed) {
    make_surface = surf.vk_create_surface;
    uint64_t sh = 0;
    if (!surf.vk_create_surface(inst, &sh)) { err = "the window's Vulkan surface could not be made"; return false; }
    surface = (VkSurfaceKHR)sh;
  }
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(inst, &n, devs.data());
  int best = -1, best_score = -1;
  uint32_t best_q = 0;
  for (uint32_t i = 0; i < n; i++) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(devs[i], &p);
    if (p.apiVersion < VK_API_VERSION_1_1) continue;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qs.data());
    for (uint32_t q = 0; q < qn; q++) {
      if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
      VkBool32 pres = VK_TRUE;
      if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface, &pres);
      if (!pres) continue;
      const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
      if (score > best_score) best = (int)i, best_score = score, best_q = q;
      break;
    }
  }
  if (best < 0) { err = "no Vulkan 1.1 GPU with a graphics queue" + std::string(surface ? " that can show the window" : ""); return false; }
  phys = devs[(size_t)best], qfam = best_q;
  {  // the queue's timestamp width (results are masked to it)
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qs.data());
    ts_bits = qfam < qn ? qs[qfam].timestampValidBits : 0;
  }
  vkGetPhysicalDeviceProperties(phys, &props);
  vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
  r->adapter_ = props.deviceName;
  VkPhysicalDeviceFeatures have{}, want{};
  vkGetPhysicalDeviceFeatures(phys, &have);
  bc_ok = have.textureCompressionBC, aniso_ok = have.samplerAnisotropy;
  want.textureCompressionBC = have.textureCompressionBC, want.samplerAnisotropy = have.samplerAnisotropy;
  // Session 98 phone port (stage C): which GPU and which texture path, for the phone's viewer.log (BC1-3 decoded on the CPU when no)
  fprintf(stderr, "vk: %s, driver 0x%x, Vulkan %u.%u, BC textures %s, anisotropy %s\n", props.deviceName, props.driverVersion,
          VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), bc_ok ? "yes" : "no (CPU decode)",
          aniso_ok ? "yes" : "no");
  const float prio = 1.f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = qfam, qci.queueCount = 1, qci.pQueuePriorities = &prio;
  const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci, dci.pEnabledFeatures = &want;
  dci.enabledExtensionCount = surface ? 1 : 0, dci.ppEnabledExtensionNames = dext;
  if ((res = vkCreateDevice(phys, &dci, nullptr, &dev)) != VK_SUCCESS) { err = VkStr("vkCreateDevice", res); return false; }
  LoadDevice(dev);
  vkGetDeviceQueue(dev, qfam, 0, &queue);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, pci.queueFamilyIndex = qfam;
  vkCreateCommandPool(dev, &pci, nullptr, &pool);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &cai, &cmd);
  vkAllocateCommandBuffers(dev, &cai, &up);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCreateFence(dev, &fci, nullptr, &fence);

  // samplers: D3D11's ANISOTROPIC x8 (wrap / clamp), MIN_MAG_MIP_LINEAR clamp, MIN_MAG_MIP_POINT clamp
  auto sampler = [&](VkSampler& s, VkFilter f, VkSamplerMipmapMode mm, VkSamplerAddressMode am, bool an) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ci.minFilter = f, ci.mipmapMode = mm, ci.addressModeU = ci.addressModeV = ci.addressModeW = am;
    ci.anisotropyEnable = an && aniso_ok ? VK_TRUE : VK_FALSE, ci.maxAnisotropy = an && aniso_ok ? std::min(8.f, props.limits.maxSamplerAnisotropy) : 1.f;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(dev, &ci, nullptr, &s);
  };
  sampler(s_aniso_wrap, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, true);
  sampler(s_aniso_clamp, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, true);
  sampler(s_lin, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);
  sampler(s_point, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);

  // descriptor set layouts (viewer_hlsl.h: the scene's VKB(binding, set); the small shaders' register shifts b0 / t1.. / s8..)
  const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  auto layout = [&](std::vector<VkDescriptorSetLayoutBinding> b) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (uint32_t)b.size(), ci.pBindings = b.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(dev, &ci, nullptr, &l);
    return l;
  };
  const VkDescriptorType UBO = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, IMG = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SMP = VK_DESCRIPTOR_TYPE_SAMPLER;
  l_scene0 = layout({{0, UBO, 1, vf, nullptr}, {1, UBO, 1, vf, nullptr}, {2, IMG, 1, vf, nullptr}, {3, IMG, 1, vf, nullptr}, {4, IMG, 1, vf, nullptr},
                     {5, IMG, 1, vf, nullptr}, {6, SMP, 1, vf, &s_aniso_wrap}, {7, SMP, 1, vf, &s_aniso_clamp}});
  l_scene1 = layout({{0, UBO, 1, vf, nullptr}, {1, UBO, 1, vf, nullptr}, {2, IMG, 1, vf, nullptr}, {3, IMG, 1, vf, nullptr}, {4, IMG, 1, vf, nullptr},
                     {5, IMG, 1, vf, nullptr}, {6, IMG, 1, vf, nullptr}, {7, IMG, 1, vf, nullptr}, {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr}});
  auto small = [&](VkSampler* s0) {
    return layout({{0, UBO, 1, vf, nullptr}, {1, IMG, 1, vf, nullptr}, {2, IMG, 1, vf, nullptr}, {3, IMG, 1, vf, nullptr}, {4, IMG, 1, vf, nullptr},
                   {5, IMG, 1, vf, nullptr}, {6, IMG, 1, vf, nullptr}, {8, SMP, 1, vf, s0}, {9, SMP, 1, vf, &s_point}});
  };
  l_small_lin = small(&s_lin);
  l_small_aniso = small(&s_aniso_clamp);
  auto playout = [&](std::vector<VkDescriptorSetLayout> ls) {
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = (uint32_t)ls.size(), ci.pSetLayouts = ls.data();
    VkPipelineLayout l = VK_NULL_HANDLE;
    vkCreatePipelineLayout(dev, &ci, nullptr, &l);
    return l;
  };
  pl_scene = playout({l_scene0, l_scene1});
  pl_small_lin = playout({l_small_lin});
  pl_small_aniso = playout({l_small_aniso});

  if (!MakeBuf(stage, 64u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) || !MakeBuf(readback, 16u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true) ||
      !MakeBuf(pick_buf, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true)) { err = "vk: staging buffers"; return false; }
  {  // identity instance + identity bone, the zero texture
    if (!MakeBuf(ident, 128, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) { err = "vk: identity buffer"; return false; }
    const assets::Matrix id = assets::Identity();
    memcpy(ident.map, id.data(), 64), memcpy((uint8_t*)ident.map + 64, id.data(), 64);
    if (!MakeImg(zero, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { err = "vk: zero texture"; return false; }
  }
  const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const VkImageUsageFlags dt = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!MakeImg(props_rt, VK_FORMAT_R32_SFLOAT, 512, 512, 1, rt) || !MakeImg(props_depth, kDepthFmt, 512, 512, 1, dt) ||
      !MakeImg(wr_depth, kDepthFmt, 1024, 1024, 1, dt) || !MakeImg(plane_a, VK_FORMAT_R8_UNORM, 512, 512, 1, rt) ||
      !MakeImg(plane_b, VK_FORMAT_R8_UNORM, 256, 256, 1, rt) || !MakeImg(plane_c, VK_FORMAT_R8_UNORM, 256, 256, 1, rt) ||
      !MakeImg(rim_a, VK_FORMAT_R8G8B8A8_UNORM, 512, 256, 1, rt) || !MakeImg(rim_b, VK_FORMAT_R8G8B8A8_UNORM, 512, 256, 1, rt) ||
      !MakeImg(rim_depth, kDepthFmt, 512, 256, 1, dt)) { err = "shadow targets failed"; return false; }
  {  // every target starts readable and zero (D3D11's textures start zeroed)
    BeginCmd();
    for (Img* im : {&zero, &props_rt, &plane_a, &plane_b, &plane_c, &rim_a, &rim_b}) ClearImg(cmd, *im, 0, 0, 0, 0), Barrier(cmd, *im, ReadLayout(*im));
    for (Img* im : {&props_depth, &wr_depth, &rim_depth}) Barrier(cmd, *im, ReadLayout(*im));
    if (!SubmitWait()) { err = "vk: first submit failed"; return false; }
  }
  {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP, qi.queryCount = 64;
    if (props.limits.timestampComputeAndGraphics && ts_bits) vkCreateQueryPool(dev, &qi, nullptr, &qpool);
  }
  if (windowed && !MakeSwapchain(w, h, err)) return false;
  return true;
}

bool RendererVk::Impl::MakeSwapchain(int w, int h, std::string& err) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, fmts.data());
  // a UNORM format (the picture is written gamma-encoded already, like D3D11's B8G8R8A8_UNORM back buffer); never an sRGB one
  auto srgb = [](VkFormat f) { return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32; };
  VkSurfaceFormatKHR sf = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
  for (auto it = fmts.rbegin(); it != fmts.rend(); ++it) if (!srgb(it->format)) sf = *it;
  for (auto& f : fmts) if (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) sf = f;
  for (auto& f : fmts) if (f.format == VK_FORMAT_R8G8B8A8_UNORM) sf = f;
  for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) sf = f;  // (D3D11's back buffer format; the picture is written as is)
  VkExtent2D ext = caps.currentExtent;
  if (ext.width == 0xFFFFFFFFu) ext = {(uint32_t)w, (uint32_t)h};
  if (ext.width == 0 || ext.height == 0) return true;  // (minimised: no swap chain until it has a size)
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface, ci.minImageCount = std::max(caps.minImageCount, 2u);
  if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
  ci.imageFormat = sf.format, ci.imageColorSpace = sf.colorSpace, ci.imageExtent = ext, ci.imageArrayLayers = 1;
  // the picture reaches the swap chain image by a copy / blit: TRANSFER_DST must be allowed
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { err = "the window's swap chain cannot be a copy target"; return false; }
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  // IDENTITY (the compositor rotates; a rotated currentTransform would make every present SUBOPTIMAL - see DrawFrame)
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
    if (caps.supportedCompositeAlpha & a) { ci.compositeAlpha = a; break; }
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR, ci.clipped = VK_TRUE;  // (D3D11: Present(1): vsync)
  VkResult res = vkCreateSwapchainKHR(dev, &ci, nullptr, &swap);
  if (res != VK_SUCCESS) { err = VkStr("vkCreateSwapchainKHR", res); return false; }
  fprintf(stderr, "vk swap chain: %ux%u, format %d, transform 0x%x (current 0x%x), %u+ images\n", ext.width, ext.height, (int)sf.format,
          (unsigned)ci.preTransform, (unsigned)caps.currentTransform, ci.minImageCount);  // (Session 98 stage C: the phone's viewer.log)
  swap_fmt = sf.format;
  vkGetSwapchainImagesKHR(dev, swap, &n, nullptr);
  swap_imgs.resize(n);
  vkGetSwapchainImagesKHR(dev, swap, &n, swap_imgs.data());
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  sem_render.resize(n);
  for (auto& s2 : sem_render) vkCreateSemaphore(dev, &si, nullptr, &s2);
  if (!sem_acquire) vkCreateSemaphore(dev, &si, nullptr, &sem_acquire);
  r->w_ = (int)ext.width, r->h_ = (int)ext.height;
  if (ci.minImageCount != swap_min) { swap_min = ci.minImageCount; if (on_swap) on_swap(swap_min); }  // (imgui_impl_vulkan: SetMinImageCount)
  if (readback.size < VkDeviceSize(ext.width) * ext.height * 4) {  // (GrabBackBuffer reads the whole window picture)
    FreeBuf(readback);
    if (!MakeBuf(readback, VkDeviceSize(ext.width) * ext.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true)) { err = "vk: readback buffer"; return false; }
  }
  return EnsureImg(win_out, kOutFmt, r->w_, r->h_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
}

void RendererVk::Impl::DestroySwapchain() {
  if (!dev) return;
  vkDeviceWaitIdle(dev);
  for (auto s2 : sem_render) vkDestroySemaphore(dev, s2, nullptr);
  sem_render.clear();
  if (swap) vkDestroySwapchainKHR(dev, swap, nullptr);
  swap = VK_NULL_HANDLE, swap_imgs.clear();
}

bool RendererVk::Impl::EnsurePost(int w, int h) {
  for (auto& st : sets)
    if (st.scene.ok() && st.w == w && st.h == h) { s = &st; st.stamp = ++tick; return true; }
  s = sets[0].stamp <= sets[1].stamp ? &sets[0] : &sets[1];  // the least recently used
  if (s->scene.ok()) {
    vkDeviceWaitIdle(dev);
    for (Img* im : {&s->scene, &s->depth, &s->near_a, &s->near_b, &s->far_a, &s->far_b, &s->bright, &s->bl_a, &s->bl_b, &s->glare}) FreeImg(*im);
  }
  s->w = w, s->h = h, s->stamp = ++tick;
  const int qw = PostQuarter(w), qh = PostQuarter(h), ew = PostEighth(w), eh = PostEighth(h);
  const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  const bool ok = MakeImg(s->scene, kSceneFmt, w, h, 1, rt) &&
                  MakeImg(s->depth, kDepthFmt, w, h, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) &&
                  MakeImg(s->near_a, kSceneFmt, qw, qh, 1, rt) && MakeImg(s->near_b, kSceneFmt, qw, qh, 1, rt) && MakeImg(s->far_a, kSceneFmt, qw, qh, 1, rt) &&
                  MakeImg(s->far_b, kSceneFmt, qw, qh, 1, rt) && MakeImg(s->bright, kSceneFmt, ew, eh, 1, rt) && MakeImg(s->bl_a, kSceneFmt, ew, eh, 1, rt) &&
                  MakeImg(s->bl_b, kSceneFmt, ew, eh, 1, rt) && MakeImg(s->glare, kSceneFmt, ew, eh, 1, rt);
  if (!ok) { s->w = s->h = 0; return false; }
  for (Img* im : {&s->near_a, &s->near_b, &s->far_a, &s->far_b, &s->bright, &s->bl_a, &s->bl_b, &s->glare}) ClearImg(cmd, *im, 0, 0, 0, 0), Barrier(cmd, *im, ReadLayout(*im));
  return true;
}

// A model / sprite texture (renderer_d3d11.cpp MakeTexture's rules: the mips that fit the data; B4G4R4A4 always to RGBA8 here; BC1-3
// decoded to RGBA8 on a GPU without BC).
bool RendererVk::Impl::UploadImage(Img& out, const assets::Texture& t, std::vector<Block>* blocks, std::vector<std::string>& warnings) {
  auto warn = [&](const std::string& m) { warnings.push_back("texture '" + t.name + "': " + m); };
  if (t.width == 0 || t.height == 0 || t.format == TexFormat::kUnknown) { warn("empty or unknown format"); return false; }
  const bool bc = t.format == TexFormat::kBC1 || t.format == TexFormat::kBC2 || t.format == TexFormat::kBC3;
  if (bc && ((t.width % 4) || (t.height % 4))) { warn("block-compressed size not a multiple of 4"); return false; }
  uint32_t mips = std::max(1u, t.mips);
  {
    uint32_t mx = std::max(t.width, t.height), maxmips = 1;
    while (mx > 1) { mx >>= 1; maxmips++; }
    mips = std::min(mips, maxmips);
  }
  size_t total = 0;
  uint32_t fit = 0;
  for (uint32_t i = 0; i < mips; i++) {
    const size_t sz = MipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (total + sz > t.data.size()) break;
    total += sz, fit++;
  }
  if (fit == 0) { warn("data shorter than mip 0"); return false; }
  if (fit < mips) warn("data holds only " + std::to_string(fit) + " of " + std::to_string(mips) + " mips");
  const bool to_rgba = t.format == TexFormat::kB4G4R4A4 || (bc && !bc_ok);
  VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
  if (!to_rgba) {
    switch (t.format) {
      case TexFormat::kBC1: f = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
      case TexFormat::kBC2: f = VK_FORMAT_BC2_UNORM_BLOCK; break;
      case TexFormat::kBC3: f = VK_FORMAT_BC3_UNORM_BLOCK; break;
      case TexFormat::kB8G8R8A8: f = VK_FORMAT_B8G8R8A8_UNORM; break;
      default: f = VK_FORMAT_R8G8B8A8_UNORM; break;
    }
  }
  if (!MakeImg(out, f, (int)t.width, (int)t.height, fit, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, blocks)) { warn("vkCreateImage failed"); return false; }
  UpBegin();
  Barrier(up, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  size_t src = 0;
  for (uint32_t i = 0; i < fit; i++) {
    const uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
    const size_t sb = MipBytes(t.format, mw, mh), db = to_rgba ? size_t(mw) * mh * 4 : sb;
    const VkDeviceSize o = StageAlloc(db);
    uint8_t* d = (uint8_t*)stage.map + o;
    if (t.format == TexFormat::kB4G4R4A4) {
      for (size_t k = 0; k < (size_t)mw * mh; k++) {
        const uint16_t px = (uint16_t)(t.data[src + 2 * k] | (t.data[src + 2 * k + 1] << 8));
        d[4 * k] = (uint8_t)(((px >> 8) & 15) * 17), d[4 * k + 1] = (uint8_t)(((px >> 4) & 15) * 17);
        d[4 * k + 2] = (uint8_t)((px & 15) * 17), d[4 * k + 3] = (uint8_t)(((px >> 12) & 15) * 17);
      }
    } else if (to_rgba) {
      DecodeBC(t.format, t.data.data() + src, mw, mh, d);
    } else {
      memcpy(d, t.data.data() + src, sb);
    }
    src += sb;
    VkBufferImageCopy c{};
    c.bufferOffset = o, c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1}, c.imageExtent = {mw, mh, 1};
    vkCmdCopyBufferToImage(up, stage.b, out.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  }
  Barrier(up, out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return true;
}

void RendererVk::Impl::CollectTimer() {
  last.clear(), last_total = 0, timer_ok = false;
  if (!qpool || marks.empty()) return;
  std::vector<uint64_t> ts(marks.size() + 1);
  if (vkGetQueryPoolResults(dev, qpool, 0, (uint32_t)ts.size(), ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return;
  const double ns = props.limits.timestampPeriod;
  for (size_t i = 0; i < marks.size(); i++) {
    const uint64_t mask = ts_bits >= 64 ? ~0ull : ((1ull << ts_bits) - 1);
    const double ms = double((ts[i + 1] - ts[i]) & mask) * ns / 1e6;
    last.push_back({marks[i], ms});
    last_total += ms;
  }
  timer_ok = true;
}

// ---- drawing ---------------------------------------------------------------------------------------------------------

// Set 0 of the scene layout: the frame constants, the shadow constants, the four shadow maps (the zero texture when `maps` is false:
// the caster passes render into them).
VkDescriptorSet RendererVk::Impl::SceneSet0(const FrameCB& fcb, bool maps) {
  VkDescriptorSet set = AllocSet(l_scene0);
  Writes w;
  w.set = set;
  w.Buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Push(&fcb, sizeof fcb));
  w.Buffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Push(&r->shadow_set_.Cb(), sizeof(ShadowCB)));
  if (maps) ImgW(w, 2, props_rt), ImgW(w, 3, wr_depth), ImgW(w, 4, plane_b), ImgW(w, 5, rim_b);
  else for (uint32_t b = 2; b <= 5; b++) ImgW(w, b, zero);
  Commit(w);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_scene, 0, 1, &set, 0, nullptr);
  return set;
}

void RendererVk::Impl::DrawMesh(const GpuModel& gm, const GpuMesh& me, bool blended) {
  DrawCB dcb;
  RendererVk::DrawTex dt;
  if (!r->MakeDrawCB(gm, me, blended, live.ok(), dcb, dt)) return;
  VkModel& d = VM(gm);
  auto tex = [&](int sl) -> const Img& { return sl >= 0 && size_t(sl) < d.tex.size() && d.tex[size_t(sl)].ok() ? d.tex[size_t(sl)] : zero; };
  VkDescriptorSet set = AllocSet(l_scene1);
  Writes w;
  w.set = set;
  w.Buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Push(&dcb, sizeof dcb));
  w.Buffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cur_pick);
  ImgW(w, 2, dt.diffuse_live ? live : tex(dt.diffuse));
  ImgW(w, 3, tex(dt.lightmap)), ImgW(w, 4, tex(dt.stage)), ImgW(w, 5, tex(dt.normal)), ImgW(w, 6, tex(dt.detail)), ImgW(w, 7, tex(dt.detail_mask));
  if (gm.has_bones && !d.bones.empty()) {
    if (d.bones_frame != frame_id) d.bones_ref = Push(d.bones.data(), d.bones.size() * sizeof(assets::Matrix), std::max<VkDeviceSize>(props.limits.minStorageBufferOffsetAlignment, 16)), d.bones_frame = frame_id;
    w.Buffer(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, d.bones_ref);
  } else {
    w.Buffer(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, {ident.b, 64, 64});
  }
  Commit(w);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_scene, 1, 1, &set, 0, nullptr);
  const GpuPlacement* pl = me.placement >= 0 ? &gm.placements[(size_t)me.placement] : nullptr;
  if (pl && pl->count == 0) return;
  const VkModel::Range& rg = d.meshes[(size_t)me.buf];
  VkBuffer vbs[2] = {d.geo.b, pl ? d.geo.b : ident.b};
  VkDeviceSize offs[2] = {rg.vb, pl ? d.placements[(size_t)me.placement] : 0};
  vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
  vkCmdBindIndexBuffer(cmd, d.geo.b, rg.ib, VK_INDEX_TYPE_UINT32);
  if (draw_inst >= 0 && pl) {  // the highlight mask draws one copy of a placed mesh
    if ((uint32_t)draw_inst < pl->count) vkCmdDrawIndexed(cmd, me.index_count, 1, 0, 0, (uint32_t)draw_inst);
    return;
  }
  vkCmdDrawIndexed(cmd, me.index_count, pl ? pl->count : 1, 0, 0, 0);
}

void RendererVk::Impl::Blur(Img& src, Img& dst, Prog prog, float dx, float dy) {
  Barrier(cmd, src, ReadLayout(src));
  BeginRP(&dst, nullptr, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  PipeKey k;
  k.prog = prog, k.color = dst.f;
  BindPipe(Pipe(k));
  const float dir[4] = {dx, dy, 0, 0};
  VkDescriptorSet set = AllocSet(l_small_lin);
  Writes w;
  w.set = set;
  w.Buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Push(dir, 16));
  ImgW(w, 1, src);
  Commit(w);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_small_lin, 0, 1, &set, 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  EndRP();
  Barrier(cmd, dst, ReadLayout(dst));
}

void RendererVk::Impl::ShadowPasses(const Camera& cam, int w, int h) {
  std::vector<float> foot_y;
  r->PrepareShadows(cam, w, h, foot_y);
  FrameCB fcb = r->MakeFrameCB(cam, w, h);
  const FrameCB main_fcb = fcb;
  auto casters = [&](int kind) {
    for (auto& g : r->models_)
      for (auto& me : g.meshes)
        if (Renderer::IsCaster(g.materials[me.material], me, kind)) DrawMesh(g, me, false);
  };
  const ShadowSetup& ss = r->shadow_set_;
  if (ss.PropsActive()) {
    memcpy(fcb.viewProj.m, ss.PropsVP().m, sizeof fcb.viewProj.m);
    const float clear[4] = {-1e4f, 0, 0, 0};
    BeginRP(&props_rt, &props_depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
    PipeKey k;
    k.prog = kPCaster, k.depth = 1, k.color = props_rt.f, k.depthf = kDepthFmt;
    BindPipe(Pipe(k));
    SceneSet0(fcb, false);
    casters(0);
    EndRP();
  }
  Mark("shadow_props");
  if (ss.WrestlerMapActive()) {
    memcpy(fcb.viewProj.m, ss.WrestlerVP().m, sizeof fcb.viewProj.m);
    BeginRP(nullptr, &wr_depth, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR);
    PipeKey k;
    k.prog = kPCaster, k.depth = 1, k.depthf = kDepthFmt;
    BindPipe(Pipe(k));
    SceneSet0(fcb, false);
    casters(1);
    EndRP();
  }
  Mark("shadow_wrestler");
  if (ss.PlaneActive()) {
    const float clear[4] = {0, 0, 0, 0};
    BeginRP(&plane_a, nullptr, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE, clear);
    PipeKey k;
    k.prog = kPPlane, k.blend = 3, k.color = plane_a.f;
    BindPipe(Pipe(k));
    SceneSet0(fcb, false);
    for (size_t mi = 0; mi < r->models_.size(); mi++) {  // one plane per standing height: each model projects onto its own
      if (!r->models_[mi].has_wrestler) continue;
      r->cur_plane_y_ = foot_y[mi];
      for (auto& me : r->models_[mi].meshes)
        if (Renderer::IsCaster(r->models_[mi].materials[me.material], me, 1)) DrawMesh(r->models_[mi], me, false);
    }
    r->cur_plane_y_ = 0.f;
    EndRP();
    Blur(plane_a, plane_b, kPBlurDown, 1.f / 512.f, 1.f / 512.f);  // 512 -> 256 (4 taps)
    Blur(plane_b, plane_c, kPBlur, 1.f / 256.f, 0);
    Blur(plane_c, plane_b, kPBlur, 0, 1.f / 256.f);  // the finished mask is plane_b
  }
  Mark("shadow_plane");
  if (ss.RimActive()) {
    const float clear[4] = {1, 0, 0, 1};
    BeginRP(&rim_a, &rim_depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
    PipeKey k;
    k.prog = kPRim, k.depth = 1, k.color = rim_a.f, k.depthf = kDepthFmt;
    BindPipe(Pipe(k));
    SceneSet0(main_fcb, false);
    casters(1);
    EndRP();
    Blur(rim_a, rim_b, kPRim13, 1.f / 512.f, 1.f / 256.f);
  }
  Mark("rim_map");
  for (Img* im : {&props_rt, &props_depth, &wr_depth, &plane_b, &rim_b}) Barrier(cmd, *im, ReadLayout(*im));
}

void RendererVk::Impl::Scene(int w, int h, const Camera& cam) {
  auto pre = [&](float v) {
    if (!(r->gamma_ && r->look_lit_)) return v;
    const float l = v < 0.081f ? v / 4.5f : std::pow((v + 0.099f) / 1.099f, 1.f / 0.45f);  // inverse BT.709
    return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;       // sRGB encode (the curve decodes sRGB)
  };
  const float clear[4] = {pre(r->clear_rgb_[0]), pre(r->clear_rgb_[1]), pre(r->clear_rgb_[2]), 1.0f};  // (Session 98 importer: SetClearColour)
  BeginRP(&s->scene, &s->depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
  (void)w, (void)h;
  const FrameCB fcb = r->MakeFrameCB(cam, w, h);
  std::vector<Renderer::DrawItem> items;
  r->BuildSceneList(cam, items);
  bool set0 = false;
  for (const Renderer::DrawItem& it : items) {
    PipeKey k;
    k.prog = kPScene, k.cull = it.raster, k.blend = it.blend, k.depth = it.depth_write ? 1 : 2, k.color = kSceneFmt, k.depthf = kDepthFmt;
    BindPipe(Pipe(k));
    if (!set0) SceneSet0(fcb, true), set0 = true;
    DrawMesh(*it.gm, *it.me, it.blended);
  }
  EndRP();
}

void RendererVk::Impl::SpritePass(const FrameCB& fcb) {
  std::vector<Renderer::SpriteVertex> vb;
  std::vector<Renderer::SpriteRun> runs;
  r->BuildSprites(fcb, sprite_tex.size(), vb, runs);
  if (vb.empty()) return;
  const BufRef v = Push(vb.data(), vb.size() * sizeof(vb[0]), 16);
  float cbv[20];
  r->SpriteConsts(fcb, cbv);
  const BufRef cb = Push(cbv, sizeof cbv);
  // Session 99 (particles 4): no depth attachment; the pixel shader reads the scene depth (its own depth test + the soft fade)
  Barrier(cmd, s->depth, ReadLayout(s->depth));
  BeginRP(&s->scene, nullptr, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  PipeKey k;
  k.prog = kPSprite, k.blend = 1, k.color = kSceneFmt;
  BindPipe(Pipe(k));
  vkCmdBindVertexBuffers(cmd, 0, 1, &v.b, &v.off);
  for (const auto& run : runs) {
    VkDescriptorSet set = AllocSet(l_small_aniso);
    Writes w;
    w.set = set;
    w.Buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cb);
    ImgW(w, 1, sprite_tex[size_t(run.tex)]);
    ImgW(w, 2, s->depth);
    Commit(w);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_small_aniso, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, (uint32_t)run.count, 1, (uint32_t)run.first, 0);
  }
  EndRP();
}

void RendererVk::Impl::IdPass(const Camera& cam, int w, int h, int sx, int sy) {
  if (!EnsureImg(id_img, VK_FORMAT_R32G32_UINT, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      !EnsureImg(id_depth, kDepthFmt, w, h, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return;
  for (bool& p : pending) p = false;  // (EnsureImg may have made a new size: the old readbacks are stale)
  const float zero4[4] = {0, 0, 0, 0};
  BeginRP(&id_img, &id_depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_CLEAR, zero4);
  if (sx >= 0 && sy >= 0) {  // hover: only the pixel under the cursor needs shading
    const VkRect2D sc{{sx, sy}, {1, 1}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
  }
  PipeKey k;
  k.prog = kPId, k.depth = 1, k.color = id_img.f, k.depthf = kDepthFmt;
  BindPipe(Pipe(k));
  SceneSet0(r->MakeFrameCB(cam, w, h), false);
  draw_inst = -1;
  for (size_t mi = 0; mi < r->models_.size(); mi++) {
    const GpuModel& gm = r->models_[mi];
    for (size_t kk = 0; kk < gm.meshes.size(); kk++) {
      if (r->PickSkip(gm, kk)) continue;
      const PickCB pc = r->MakePickCB(mi, kk, true);
      cur_pick = Push(&pc, sizeof pc);
      DrawMesh(gm, gm.meshes[kk], false);
    }
  }
  PickCB zero_pick{};
  cur_pick = Push(&zero_pick, sizeof zero_pick);
  EndRP();
}

void RendererVk::Impl::QueueReadback(int x, int y) {
  if (!id_img.ok() || x < 0 || y < 0 || x >= id_img.w || y >= id_img.h) return;
  int sl = -1;
  for (int i = 0; i < 3; i++)
    if (!pending[i]) { sl = i; break; }
  if (sl < 0) return;
  Barrier(cmd, id_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy c{};
  c.bufferOffset = VkDeviceSize(sl) * 8, c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.imageOffset = {x, y, 0}, c.imageExtent = {1, 1, 1};
  vkCmdCopyImageToBuffer(cmd, id_img.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pick_buf.b, 1, &c);
  HostReadBarrier(cmd);
  pending[sl] = true, pend_seq[sl] = ++seq, pend_gen[sl] = r->models_gen_;
}

// (every frame's commands finished before the next frame starts: a queued readback is always ready here)
void RendererVk::Impl::PollReadback() {
  unsigned best_seq = 0;
  bool got = false;
  PickHit best;
  bool any = false;
  for (bool p : pending) any = any || p;
  if (any) Invalidate(pick_buf);
  for (int i = 0; i < 3; i++) {
    if (!pending[i]) continue;
    pending[i] = false;
    if (pend_gen[i] != r->models_gen_) continue;
    const uint32_t* px = (const uint32_t*)((const uint8_t*)pick_buf.map + 8 * i);
    const PickHit hit = r->DecodeId(px[0], px[1]);
    if (!got || pend_seq[i] > best_seq) best = hit, best_seq = pend_seq[i], got = true;
  }
  if (got) r->hover_hit_ = best;
}

void RendererVk::Impl::Post(Img& out, const std::vector<PostStep>& plan) {
  auto tex = [&](PostTarget t) -> Img* {
    switch (t) {
      case PostTarget::kScene: return &s->scene;
      case PostTarget::kDepth: return &s->depth;
      case PostTarget::kNearA: return &s->near_a;
      case PostTarget::kNearB: return &s->near_b;
      case PostTarget::kFarA: return &s->far_a;
      case PostTarget::kFarB: return &s->far_b;
      case PostTarget::kBright: return &s->bright;
      case PostTarget::kBlA: return &s->bl_a;
      case PostTarget::kBlB: return &s->bl_b;
      case PostTarget::kGlare: return &s->glare;
      case PostTarget::kOut: return &out;
      default: return nullptr;
    }
  };
  for (const PostStep& st : plan) {
    if (st.mark) { Mark(st.mark); continue; }
    Img* dst = tex(st.dst);
    VkDescriptorSet set = AllocSet(l_small_lin);
    Writes w;
    w.set = set;
    w.Buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Push(&st.cb, sizeof st.cb));
    for (int i = 0; i < 6; i++) {  // (D3D11 binds null for the slots past nsrc; here the zero texture)
      Img* src = i < st.nsrc ? tex(st.src[i]) : nullptr;
      if (src) Barrier(cmd, *src, ReadLayout(*src));
      ImgW(w, uint32_t(1 + i), src ? *src : zero);
    }
    Commit(w);
    BeginRP(dst, nullptr, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    PipeKey k;
    k.prog = uint8_t(kPPost0 + (int)st.ps), k.color = dst->f;
    BindPipe(Pipe(k));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_small_lin, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    EndRP();
    if (dst != &out) Barrier(cmd, *dst, ReadLayout(*dst));
  }
}

void RendererVk::Impl::FlashPass(Img& out, const FrameCB& fcb) {
  std::vector<Renderer::FlashDraw> draws;
  r->BuildFlashes(fcb, draws);
  if (draws.empty()) return;
  Barrier(cmd, s->depth, ReadLayout(s->depth));
  BeginRP(&out, nullptr, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  PipeKey k;
  k.prog = kPFlash, k.blend = 2, k.color = out.f;
  BindPipe(Pipe(k));
  const GpuMesh* bound = nullptr;
  for (const Renderer::FlashDraw& fd : draws) {
    const VkModel& d = VM(*fd.gm);
    if (fd.me != bound) {
      bound = fd.me;
      const VkModel::Range& rg = d.meshes[(size_t)fd.me->buf];
      vkCmdBindVertexBuffers(cmd, 0, 1, &d.geo.b, &rg.vb);
      vkCmdBindIndexBuffer(cmd, d.geo.b, rg.ib, VK_INDEX_TYPE_UINT32);
    }
    const int ds = fd.gm->materials[fd.me->material].diffuse;
    VkDescriptorSet set = AllocSet(l_small_aniso);
    Writes w;
    w.set = set;
    w.Buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Push(&fd.cb, sizeof fd.cb));
    ImgW(w, 1, ds >= 0 && d.tex[(size_t)ds].ok() ? d.tex[(size_t)ds] : zero);
    ImgW(w, 2, s->depth);
    Commit(w);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_small_aniso, 0, 1, &set, 0, nullptr);
    vkCmdDrawIndexed(cmd, fd.me->index_count, 1, 0, 0, 0);
  }
  EndRP();
}

void RendererVk::Impl::HighlightPass(Img& out, int w, int h, const FrameCB& fcb) {
  bool sel = false, hov = false;
  if (!r->HighlightWanted(sel, hov)) return;
  if (!EnsureImg(mask, VK_FORMAT_R8G8_UNORM, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return;
  const float zero4[4] = {0, 0, 0, 0};
  BeginRP(&mask, &s->depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_LOAD, zero4);  // the scene's own depth: a part behind something shows only its visible area
  bool set0 = false;
  auto draw = [&](const PickHit& p, uint8_t blend) {
    const GpuModel& gm = r->models_[(size_t)p.model];
    const GpuMesh& me = gm.meshes[(size_t)p.mesh];
    PipeKey k;
    k.prog = kPMask, k.blend = blend, k.depth = 2, k.color = mask.f, k.depthf = kDepthFmt;
    BindPipe(Pipe(k));
    if (!set0) SceneSet0(fcb, false), set0 = true;
    const PickCB pc = r->MakePickCB((size_t)p.model, (size_t)p.mesh, false);
    cur_pick = Push(&pc, sizeof pc);
    draw_inst = p.instance;
    DrawMesh(gm, me, false);
    draw_inst = -1;
  };
  if (sel) draw(r->hl_sel_, 5);
  if (hov) draw(r->hl_hover_, 6);
  PickCB zero_pick{};
  cur_pick = Push(&zero_pick, sizeof zero_pick);
  EndRP();
  // composite onto the finished picture
  const CompCB cc = r->MakeCompCB(w, h, sel, hov);
  VkDescriptorSet set = AllocSet(l_small_lin);
  Writes wr;
  wr.set = set;
  wr.Buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Push(&cc, sizeof cc));
  Barrier(cmd, mask, ReadLayout(mask));
  ImgW(wr, 1, mask);
  Commit(wr);
  BeginRP(&out, nullptr, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  PipeKey k;
  k.prog = kPComp, k.blend = 4, k.color = out.f;
  BindPipe(Pipe(k));
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_small_lin, 0, 1, &set, 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  EndRP();
  Mark("highlight");
}

void RendererVk::Impl::Frame(Img& out, int w, int h, const Camera& cam, bool window) {
  if (!EnsurePost(w, h)) { fprintf(stderr, "post targets failed\n"); return; }
  if (timer_on && qpool) {
    marks.clear();
    vkCmdResetQueryPool(cmd, qpool, 0, 64);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 0);
  }
  ShadowPasses(cam, w, h);
  Scene(w, h, cam);
  if (!r->sprites_.empty() && r->debug_ == 0) SpritePass(r->MakeFrameCB(cam, w, h));  // after the scene, before post
  Mark("scene");
  PollReadback();
  {
    const bool hover_now = r->hover_req_ && window && w == r->w_ && h == r->h_;
    if (hover_now || (r->timing_pick_ && timer_on)) {
      IdPass(cam, w, h, hover_now ? r->hover_x_ : -1, hover_now ? r->hover_y_ : -1);
      if (hover_now) QueueReadback(r->hover_x_, r->hover_y_);
      r->hover_req_ = false;
      Mark("pick_id");
    }
  }
  // the live screen: keep this frame's pre-post scene colour for the next frame's big-screen draws
  if (r->live_screen_ && r->HasScreen()) {
    if (!live.ok() || live.w != w || live.h != h) {
      if (live.ok()) { SubmitWait(); vkDeviceWaitIdle(dev); FreeImg(live); BeginCmd(); }
      if (MakeImg(live, kSceneFmt, w, h, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ClearImg(cmd, live, 0, 0, 0, 1), live_has = false;
    }
    if (live.ok()) {
      Barrier(cmd, s->scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      Barrier(cmd, live, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkImageCopy c{};
      c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.extent = {(uint32_t)w, (uint32_t)h, 1};
      vkCmdCopyImage(cmd, s->scene.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, live.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
      Barrier(cmd, live, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      live_has = true;
    }
  }
  const FrameCB fcb = r->MakeFrameCB(cam, w, h);
  Post(out, BuildPostPlan(r->MakePostParams(fcb), w, h));
  if (r->FlashesWanted()) FlashPass(out, fcb);  // over the finished picture
  if ((r->hl_hover_.valid() || r->hl_sel_.valid()) && (window || (r->timing_pick_ && timer_on))) HighlightPass(out, w, h, fcb);  // before the UI
}

bool RendererVk::Impl::ReadImage(Img& im, std::vector<uint8_t>& px) {
  const VkDeviceSize bytes = VkDeviceSize(im.w) * im.h * 4;
  if (readback.size < bytes) {
    SubmitWait();  // (nothing reads `readback` yet; the frame so far is kept)
    FreeBuf(readback);
    if (!MakeBuf(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true)) return false;
    BeginCmd();
    return false;  // (the caller re-renders: the frame was submitted without the copy)
  }
  Barrier(cmd, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy c{};
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.imageExtent = {(uint32_t)im.w, (uint32_t)im.h, 1};
  vkCmdCopyImageToBuffer(cmd, im.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.b, 1, &c);
  HostReadBarrier(cmd);
  if (!SubmitWait()) return false;
  Invalidate(readback);
  px.resize((size_t)bytes);
  memcpy(px.data(), readback.map, (size_t)bytes);
  return true;
}

// ---- RendererVk -------------------------------------------------------------------------------------------------------

RendererVk::RendererVk() : p_(new Impl(this)) {}

RendererVk::~RendererVk() {
  Impl& m = *p_;
  if (!m.dev) return;
  vkDeviceWaitIdle(m.dev);
  models_.clear();
  for (Img& t : m.sprite_tex) m.FreeImg(t);
  for (auto& kv : m.ui_imgs) m.FreeImg(kv.second);
  for (Impl::PostSet& st : m.sets)
    for (Img* im : {&st.scene, &st.depth, &st.near_a, &st.near_b, &st.far_a, &st.far_b, &st.bright, &st.bl_a, &st.bl_b, &st.glare}) m.FreeImg(*im);
  for (Img* im : {&m.zero, &m.props_rt, &m.props_depth, &m.wr_depth, &m.plane_a, &m.plane_b, &m.plane_c, &m.rim_a, &m.rim_b, &m.rim_depth, &m.live,
                  &m.win_out, &m.shot_out, &m.id_img, &m.id_depth, &m.mask}) m.FreeImg(*im);
  for (Buf& b : m.ring) m.FreeBuf(b);
  for (Buf* b : {&m.stage, &m.readback, &m.ident, &m.pick_buf}) m.FreeBuf(*b);
  for (VkFramebuffer f : m.fb_dead) vkDestroyFramebuffer(m.dev, f, nullptr);
  for (auto& kv : m.pipes) vkDestroyPipeline(m.dev, kv.second, nullptr);
  for (auto& kv : m.rps) vkDestroyRenderPass(m.dev, kv.second, nullptr);
  for (auto& kv : m.mods) vkDestroyShaderModule(m.dev, kv.second, nullptr);
  for (VkDescriptorPool p : m.dpools) vkDestroyDescriptorPool(m.dev, p, nullptr);
  m.DestroySwapchain();
  if (m.sem_acquire) vkDestroySemaphore(m.dev, m.sem_acquire, nullptr);
  if (m.qpool) vkDestroyQueryPool(m.dev, m.qpool, nullptr);
  for (VkPipelineLayout l : {m.pl_scene, m.pl_small_lin, m.pl_small_aniso}) vkDestroyPipelineLayout(m.dev, l, nullptr);
  for (VkDescriptorSetLayout l : {m.l_scene0, m.l_scene1, m.l_small_lin, m.l_small_aniso}) vkDestroyDescriptorSetLayout(m.dev, l, nullptr);
  for (VkSampler s2 : {m.s_aniso_wrap, m.s_aniso_clamp, m.s_lin, m.s_point}) vkDestroySampler(m.dev, s2, nullptr);
  vkDestroyFence(m.dev, m.fence, nullptr);
  vkDestroyCommandPool(m.dev, m.pool, nullptr);
  vkDestroyDevice(m.dev, nullptr);
  if (m.surface) vkDestroySurfaceKHR(m.inst, m.surface, nullptr);
  vkDestroyInstance(m.inst, nullptr);
}

bool RendererVk::Init(const Surface& s, int w, int h, std::string& err) {
  w_ = w, h_ = h;
  return p_->Init(s, w, h, err);
}

void RendererVk::Resize(int w, int h) {
  Impl& m = *p_;
  if (!m.windowed || !m.surface || w <= 0 || h <= 0) return;
  if (m.swap && w == w_ && h == h_) return;
  m.RecreateSwap(w, h);
}

// Session 98 review (stage B): the swap chain is remade on OUT_OF_DATE or a real size change only (with the IDENTITY transform a
// rotated phone reports SUBOPTIMAL for ever); a lost surface (Android background) gets a new one from the window.
bool RendererVk::Impl::SurfaceSizeChanged() {
  VkSurfaceCapabilitiesKHR caps;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps) != VK_SUCCESS) return false;
  return caps.currentExtent.width != 0xFFFFFFFFu && (int(caps.currentExtent.width) != r->w_ || int(caps.currentExtent.height) != r->h_);
}
void RendererVk::Impl::RecreateSwap(int w, int h) {
  DestroySwapchain();
  std::string e;
  if (!MakeSwapchain(w, h, e)) fprintf(stderr, "vk swap chain: %s\n", e.c_str());
}
void RendererVk::Impl::DropSurface() {
  DestroySwapchain();
  if (surface) vkDestroySurfaceKHR(inst, surface, nullptr);
  surface = VK_NULL_HANDLE;
}
bool RendererVk::Impl::NewSurface() {
  DropSurface();
  uint64_t sh = 0;
  if (!make_surface || !make_surface(inst, &sh)) return false;
  surface = (VkSurfaceKHR)sh;
  VkBool32 pres = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, surface, &pres);
  if (!pres) { DropSurface(); return false; }
  RecreateSwap(r->w_, r->h_);
  return swap != VK_NULL_HANDLE;
}
// After a failed submit the acquire semaphore stays signalled with nobody waiting: replace it.
void RendererVk::Impl::ResetAcquire() {
  vkDeviceWaitIdle(dev);
  if (sem_acquire) vkDestroySemaphore(dev, sem_acquire, nullptr);
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  vkCreateSemaphore(dev, &si, nullptr, &sem_acquire);
}

void RendererVk::SurfaceGone() {
  if (!p_->windowed || !p_->dev) return;
  p_->surface_away = true;
  p_->DropSurface();
}
// (a failed SurfaceBack is not final: DrawFrame retries a new surface every 30 frames while the app is in front)
bool RendererVk::SurfaceBack() {
  if (!p_->windowed || !p_->dev) return false;
  p_->surface_away = false, p_->surface_retry = 0;
  return p_->NewSurface();
}
void RendererVk::SetSwapchainHook(std::function<void(uint32_t)> f) { p_->on_swap = std::move(f); }

void RendererVk::DrawFrame(const Camera& cam) {
  Impl& m = *p_;
  if (!m.windowed || lost_) return;
  if (!m.surface) {  // Session 98 stage C review: in front with no surface (a failed SurfaceBack / NewSurface): try again now and then
    if (m.surface_away || ++m.surface_retry % 30 != 0) return;
    const bool ok = m.NewSurface();
    fprintf(stderr, "vk: surface retry %d: %s\n", m.surface_retry / 30, ok ? "ok" : "failed");
    if (!m.surface) return;
  }
  if (!m.swap) { m.RecreateSwap(w_, h_); if (!m.swap) return; }
  uint32_t idx = 0;
  VkResult res = vkAcquireNextImageKHR(m.dev, m.swap, UINT64_MAX, m.sem_acquire, VK_NULL_HANDLE, &idx);
  if (res == VK_ERROR_OUT_OF_DATE_KHR) { m.RecreateSwap(w_, h_); return; }
  if (res == VK_ERROR_SURFACE_LOST_KHR) { m.NewSurface(); return; }
  // (SUBOPTIMAL here: this frame is still drawn and presented; the present's result decides on a new swap chain)
  if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) { m.Check(res); return; }
  m.BeginCmd();
  m.Frame(m.win_out, w_, h_, cam, true);
  if (overlay_) {  // the Arena Viewer UI, drawn over the finished frame
    m.BeginRP(&m.win_out, nullptr, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    m.in_overlay = true;
    overlay_();
    m.in_overlay = false;
    m.EndRP();
  }
  {  // the picture -> the swap chain image
    m.Barrier(m.cmd, m.win_out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Img sw;
    sw.i = m.swap_imgs[idx], sw.f = m.swap_fmt, sw.w = w_, sw.h = h_;
    m.Barrier(m.cmd, sw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (sw.f == m.win_out.f) {
      VkImageCopy c{};
      c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.extent = {(uint32_t)w_, (uint32_t)h_, 1};
      vkCmdCopyImage(m.cmd, m.win_out.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sw.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else {
      VkImageBlit b{};
      b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      b.srcOffsets[1] = b.dstOffsets[1] = {w_, h_, 1};
      vkCmdBlitImage(m.cmd, m.win_out.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sw.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    m.Barrier(m.cmd, sw, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  if (!m.SubmitWait(m.sem_acquire, m.sem_render[idx])) { if (!lost_) m.ResetAcquire(); return; }
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1, pi.pWaitSemaphores = &m.sem_render[idx], pi.swapchainCount = 1, pi.pSwapchains = &m.swap, pi.pImageIndices = &idx;
  res = vkQueuePresentKHR(m.queue, &pi);
  if (res == VK_ERROR_OUT_OF_DATE_KHR || (res == VK_SUBOPTIMAL_KHR && m.SurfaceSizeChanged())) m.RecreateSwap(w_, h_);
  else if (res == VK_ERROR_SURFACE_LOST_KHR) m.NewSurface();
  else if (res != VK_SUBOPTIMAL_KHR) m.Check(res);
}

bool RendererVk::GrabBackBuffer(std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) {
  Impl& m = *p_;
  if (!m.windowed || !m.win_out.ok()) { err = "no window"; return false; }
  if (!m.in_overlay) { err = "the back buffer is read from the overlay only"; return false; }
  // Inside the overlay's render pass: end it, read the picture (submit + wait), then carry on in a new command buffer.
  m.EndRP();
  m.Barrier(m.cmd, m.win_out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkDeviceSize bytes = VkDeviceSize(m.win_out.w) * m.win_out.h * 4;
  bool ok = m.readback.size >= bytes;
  if (ok) {
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.imageExtent = {(uint32_t)m.win_out.w, (uint32_t)m.win_out.h, 1};
    vkCmdCopyImageToBuffer(m.cmd, m.win_out.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.readback.b, 1, &c);
    m.HostReadBarrier(m.cmd);
  }
  // (the acquire semaphore is waited by the final submit, the one that writes the swap chain image)
  ok = m.SubmitWait() && ok;
  if (ok) {
    m.Invalidate(m.readback);
    w = m.win_out.w, h = m.win_out.h;
    bgra.resize((size_t)bytes);
    memcpy(bgra.data(), m.readback.map, (size_t)bytes);
    for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
  } else {
    err = m.readback.size < bytes ? "readback buffer too small" : "grab submit failed";
  }
  m.BeginCmd();
  m.BeginRP(&m.win_out, nullptr, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  return ok;
}

bool RendererVk::ShotPixels(const Camera& cam, int w, int h, std::vector<uint8_t>& px, std::string& err) {
  Impl& m = *p_;
  if (w < 16 || h < 16 || w > 8192 || h > 8192) { err = "shot size out of range (16..8192)"; return false; }
  if (!m.EnsureImg(m.shot_out, kOutFmt, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) { err = "shot targets failed"; return false; }
  for (int attempt = 0; attempt < 2; attempt++) {
    m.BeginCmd();
    if (live_screen_ && !entrance_ && HasScreen()) m.Frame(m.shot_out, w, h, cam, false);  // the screen shows the previous frame: render once to seed it
    m.Frame(m.shot_out, w, h, cam, false);
    if (m.ReadImage(m.shot_out, px)) {
      for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
      return true;
    }
    if (m.recording) m.SubmitWait();
    if (lost_) break;
  }
  err = lost_ ? "device lost" : "shot readback failed";
  return false;
}

std::string RendererVk::Timing(const Camera& cam, int w, int h, int frames, std::string& err, bool with_pick) {
  Impl& m = *p_;
  if (w < 16 || h < 16 || w > 8192 || h > 8192) { err = "size out of range (16..8192)"; return ""; }
  if (!m.qpool) { err = "the GPU gave no timestamps"; return ""; }
  frames = std::max(2, std::min(frames, 200));
  if (!m.EnsureImg(m.shot_out, kOutFmt, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) { err = "timing target"; return ""; }
  m.timer_on = true;
  const PickHit save_hover = hl_hover_, save_sel = hl_sel_;
  if (with_pick) {
    timing_pick_ = true;
    for (size_t mi = 0; mi < models_.size() && !hl_sel_.valid(); mi++)
      if (!models_[mi].meshes.empty()) hl_sel_ = PickHit{(int)mi, 0, 0};
  }
  std::vector<std::pair<std::string, double>> sum;
  double total = 0;
  int counted = 0;
  for (int f = 0; f < frames; f++) {
    m.BeginCmd();
    m.Frame(m.shot_out, w, h, cam, false);
    m.SubmitWait();
    if (f == 0) continue;
    if (!m.timer_ok || m.last.empty()) continue;
    if (sum.empty()) for (auto& p : m.last) sum.push_back({p.first, 0.0});
    for (size_t i = 0; i < m.last.size() && i < sum.size(); i++) sum[i].second += m.last[i].second;
    total += m.last_total;
    counted++;
  }
  m.timer_on = false;
  timing_pick_ = false;
  hl_hover_ = save_hover; hl_sel_ = save_sel;
  if (!counted) { err = "the GPU gave no timestamps"; return ""; }
  return TimingJson(w, h, counted, total, sum);
}

PickHit RendererVk::PickRead(const Camera& cam, int w, int h, int x, int y, std::string& err) {
  Impl& m = *p_;
  m.BeginCmd();
  m.IdPass(cam, w, h, -1, -1);
  if (!m.id_img.ok()) { m.SubmitWait(); err = "the ID target could not be made"; return PickHit(); }
  for (bool& p : m.pending) p = false;
  m.Barrier(m.cmd, m.id_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy c{};
  c.bufferOffset = 16, c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.imageOffset = {x, y, 0}, c.imageExtent = {1, 1, 1};
  vkCmdCopyImageToBuffer(m.cmd, m.id_img.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.pick_buf.b, 1, &c);
  m.HostReadBarrier(m.cmd);
  if (!m.SubmitWait()) { err = "pick readback failed"; return PickHit(); }
  m.Invalidate(m.pick_buf);
  const uint32_t* px = (const uint32_t*)((const uint8_t*)m.pick_buf.map + 16);
  return DecodeId(px[0], px[1]);
}

void RendererVk::DropPendingReadbacks() { for (bool& p : p_->pending) p = false; }
bool RendererVk::LiveScreenReady() const { return p_->live.ok(); }

void RendererVk::BeforeModelsDrop() {
  if (p_->dev) vkDeviceWaitIdle(p_->dev);
}

// ---- model objects ---------------------------------------------------------------------------------------------------

std::unique_ptr<GpuModelData> RendererVk::NewModelData() {
  auto d = std::make_unique<VkModel>();
  d->im = p_.get();
  return d;
}

bool RendererVk::UploadTexture(GpuModelData& d, const assets::Texture& t, std::vector<std::string>& warnings) {
  auto& vm = static_cast<VkModel&>(d);
  vm.tex.emplace_back();
  vm.ui_ids.push_back(0);
  return p_->UploadImage(vm.tex.back(), t, &vm.blocks, warnings);
}

bool RendererVk::UploadPlacement(GpuModelData& d, const std::vector<assets::Matrix>& transforms, std::string& err) {
  (void)err;
  auto& vm = static_cast<VkModel&>(d);
  vm.placements.push_back(transforms.empty() ? 0 : vm.Append(transforms.data(), transforms.size() * sizeof(assets::Matrix)));
  return true;
}

bool RendererVk::UploadMesh(GpuModelData& d, const assets::Mesh& me, int& buf, std::string& err) {
  (void)err;
  auto& vm = static_cast<VkModel&>(d);
  VkModel::Range rg;
  rg.vb = vm.Append(me.vertices.data(), me.vertices.size() * sizeof(assets::Vertex));
  rg.ib = vm.Append(me.indices.data(), me.indices.size() * 4);
  buf = (int)vm.meshes.size();
  vm.meshes.push_back(rg);
  return true;
}

bool RendererVk::FinishModel(GpuModelData& d, std::string& err) {
  Impl& m = *p_;
  auto& vm = static_cast<VkModel&>(d);
  if (!vm.cpu.empty()) {
    if (!m.MakeBuf(vm.geo, vm.cpu.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) {
      err = "vk: geometry buffer";
      return false;
    }
    m.UpBegin();
    VkDeviceSize done = 0;
    while (done < vm.cpu.size()) {  // (in staging-sized pieces)
      const VkDeviceSize n = std::min<VkDeviceSize>(vm.cpu.size() - done, 32u << 20);
      const VkDeviceSize o = m.StageAlloc(n);
      memcpy((uint8_t*)m.stage.map + o, vm.cpu.data() + done, (size_t)n);
      VkBufferCopy c{o, done, n};
      vkCmdCopyBuffer(m.up, m.stage.b, vm.geo.b, 1, &c);
      done += n;
    }
    std::vector<uint8_t>().swap(vm.cpu);
  }
  if (!m.UpFlush()) { err = "vk: upload failed"; return false; }
  return true;
}

bool RendererVk::UploadBones(GpuModel& g, const std::vector<assets::Matrix>& full, std::string& err) {
  (void)err;
  VkModel& d = VM(g);
  d.bones = full;
  d.bones_frame = ~0ull;
  g.bone_count = full.size();
  g.has_bones = true;
  return true;
}

void RendererVk::DropBones(GpuModel& g) {
  VkModel& d = VM(g);
  d.bones.clear();
  d.bones_frame = ~0ull;
}

bool RendererVk::UpdateTexture(GpuModel& g, size_t slot, int w, int h, const uint8_t* rgba) {
  Impl& m = *p_;
  VkModel& d = VM(g);
  if (slot >= d.tex.size() || !d.tex[slot].ok()) return false;
  Img& im = d.tex[slot];
  m.UpBegin();
  const VkDeviceSize o = m.StageAlloc(VkDeviceSize(w) * h * 4);
  memcpy((uint8_t*)m.stage.map + o, rgba, size_t(w) * h * 4);
  m.Barrier(m.up, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy c{};
  c.bufferOffset = o, c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
  vkCmdCopyBufferToImage(m.up, m.stage.b, im.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  m.Barrier(m.up, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return m.UpFlush();
}

int RendererVk::AddSpriteTexture(const assets::Texture& t) {
  Impl& m = *p_;
  std::vector<std::string> warn;
  Img im;
  if (!m.UploadImage(im, t, nullptr, warn) || !m.UpFlush()) { m.FreeImg(im); return -1; }
  m.sprite_tex.push_back(im);
  return int(m.sprite_tex.size()) - 1;
}

void RendererVk::DropSpriteTextures() {
  Impl& m = *p_;
  if (m.sprite_tex.empty()) return;
  vkDeviceWaitIdle(m.dev);
  for (Img& t : m.sprite_tex) m.FreeImg(t);
  m.sprite_tex.clear();
}

uint64_t RendererVk::UiTexture(size_t model, size_t tex) {
  Impl& m = *p_;
  if (!m.ui_add || model >= models_.size()) return 0;
  VkModel& d = VM(models_[model]);
  if (tex >= d.tex.size() || !d.tex[tex].ok()) return 0;
  if (!d.ui_ids[tex]) d.ui_ids[tex] = m.ui_add(d.tex[tex].v);
  return d.ui_ids[tex];
}

// ---- for the UI backend ----
VkInstance RendererVk::Instance() const { return p_->inst; }
VkPhysicalDevice RendererVk::PhysicalDevice() const { return p_->phys; }
VkDevice RendererVk::Device() const { return p_->dev; }
uint32_t RendererVk::QueueFamily() const { return p_->qfam; }
VkQueue RendererVk::Queue() const { return p_->queue; }
VkRenderPass RendererVk::UiRenderPass() { return p_->RenderPass(kOutFmt, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE); }
VkCommandBuffer RendererVk::UiCommandBuffer() const { return p_->cmd; }
uint32_t RendererVk::SwapImageCount() const { return std::max<uint32_t>(2, (uint32_t)p_->swap_imgs.size()); }
void RendererVk::SetUiTextureHooks(std::function<uint64_t(VkImageView)> add, std::function<void(uint64_t)> remove) {
  p_->ui_add = std::move(add), p_->ui_remove = std::move(remove);
}
VkImageView RendererVk::MakeUiImage(const uint8_t* px, int w, int h, bool bc3) {
  Impl& m = *p_;
  assets::Texture t;
  t.name = "ui", t.width = (uint32_t)w, t.height = (uint32_t)h, t.mips = 1, t.format = bc3 ? TexFormat::kBC3 : TexFormat::kR8G8B8A8;
  t.data.assign(px, px + (bc3 ? size_t((w + 3) / 4) * ((h + 3) / 4) * 16 : size_t(w) * h * 4));
  std::vector<std::string> warn;
  Img im;
  if (!m.UploadImage(im, t, nullptr, warn) || !m.UpFlush()) { m.FreeImg(im); return VK_NULL_HANDLE; }
  m.ui_imgs[im.v] = im;
  return im.v;
}
void RendererVk::FreeUiImage(VkImageView view) {
  Impl& m = *p_;
  auto it = m.ui_imgs.find(view);
  if (it == m.ui_imgs.end()) return;
  vkDeviceWaitIdle(m.dev);
  m.FreeImg(it->second);
  m.ui_imgs.erase(it);
}

}  // namespace view
