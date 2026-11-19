// Session 103 lane PS1: rig_metrics.h's picture compare as ONE Vulkan compute dispatch per panel, on the renderer's own device and queue (Renderer::RawVulkan(), the GPU idle).
// The panel, the reference picture and the covered-pixel mask are copied into host-visible buffers, tools\scene\rig_metrics.comp sums |diff|, the channel sums and the washed count into a 9-word result, the host
// turns the words into a rig_metrics::Row. Equal to MeasureCpu: counts and channel sums are exact integers; only the washed test (per pixel, float vs double) may differ on a pixel exactly on its line.
// Not for pictures where px * 765 overflows 32 bits (> 5.6 million pixels): Usable() says so and the caller measures on the CPU.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer/vk_api.h"
#include "rig_metrics.h"
#include "rig_metrics_spv.h"

namespace rig_metrics_gpu {
using namespace rdr::vk;   // the renderer's loaded entry points (vk_api.h): the renderer's own device loaded them

class Gpu {
 public:
  ~Gpu() { Destroy(); }
  // px = w * h of every picture this object will measure. False (and why()) when anything is missing: the caller falls back to the CPU.
  bool Init(rdr::Renderer& renderer, size_t px) {
    Destroy();
    if (px == 0 || px * 765ull > 0xFFFFFFFFull) return Fail("picture too large for the 32-bit sums");
    renderer.WaitIdle();
    const rdr::Renderer::RawVk h = renderer.RawVulkan();
    if (!h.device) return Fail("no device");
    phys_ = VkPhysicalDevice(h.physical), dev_ = VkDevice(h.device), queue_ = VkQueue(h.queue), family_ = h.queue_family, px_ = px;
    if (!vkCreateComputePipelines || !vkCmdDispatch) return Fail("compute entry points not loaded");
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    mem_props_ = mp;
    if (!MakeBuffer(px * 4, ours_) || !MakeBuffer(px * 4, ref_) || !MakeBuffer((px + 3) / 4 * 4, mask_) || !MakeBuffer(16 * 4, out_)) return Fail("buffers");
    VkDescriptorSetLayoutBinding b[4] = {};
    for (uint32_t i = 0; i < 4; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_) != VK_SUCCESS) return Fail("set layout");
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &set_layout_, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev_, &pli, nullptr, &layout_) != VK_SUCCESS) return Fail("pipeline layout");
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kSpv_rig_metrics_comp), mi.pCode = kSpv_rig_metrics_comp;
    if (vkCreateShaderModule(dev_, &mi, nullptr, &module_) != VK_SUCCESS) return Fail("shader module");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module_, "main", nullptr};
    ci.layout = layout_;
    if (vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_) != VK_SUCCESS) return Fail("pipeline");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1, pi.poolSizeCount = 1, pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev_, &pi, nullptr, &pool_) != VK_SUCCESS) return Fail("descriptor pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_, ai.descriptorSetCount = 1, ai.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(dev_, &ai, &set_) != VK_SUCCESS) return Fail("descriptor set");
    VkDescriptorBufferInfo bi[4] = {{ours_.buf, 0, VK_WHOLE_SIZE}, {ref_.buf, 0, VK_WHOLE_SIZE}, {mask_.buf, 0, VK_WHOLE_SIZE}, {out_.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstSet = set_, w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(dev_, 4, w, 0, nullptr);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = family_, cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(dev_, &cpi, nullptr, &cmd_pool_) != VK_SUCCESS) return Fail("command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool_, cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev_, &cai, &cmd_) != VK_SUCCESS) return Fail("command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev_, &fi, nullptr, &fence_) != VK_SUCCESS) return Fail("fence");
    ready_ = true;
    return true;
  }
  bool ready() const { return ready_; }
  const std::string& why() const { return why_; }
  // The reference picture (RGB8, px pixels): made resident for the measurements that follow.
  void SetRef(const uint8_t* rgb) {
    uint8_t* d = static_cast<uint8_t*>(ref_.map);
    for (size_t i = 0; i < px_; ++i) d[i * 4] = rgb[i * 3], d[i * 4 + 1] = rgb[i * 3 + 1], d[i * 4 + 2] = rgb[i * 3 + 2], d[i * 4 + 3] = 255;
  }
  // The panel (RGBA8, px pixels) against the resident reference over `mask` (px bytes, null = every pixel).
  bool Measure(const uint8_t* ours_rgba, const uint8_t* mask, rig_metrics::Row& row) {
    if (!ready_) return false;
    std::memcpy(ours_.map, ours_rgba, px_ * 4);
    if (mask) std::memcpy(mask_.map, mask, px_);
    std::memset(out_.map, 0, 16 * 4);
    const uint32_t groups = 128;
    const uint32_t pc[3] = {uint32_t(px_), mask ? 1u : 0u, groups};
    vkResetFences(dev_, 1, &fence_);
    VkCommandBufferBeginInfo bgi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bgi);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cmd_, groups, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &cmd_;
    if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return Fail("submit");
    if (vkWaitForFences(dev_, 1, &fence_, VK_TRUE, 30ull * 1000000000ull) != VK_SUCCESS) return Fail("fence wait");
    const uint32_t* r = static_cast<const uint32_t*>(out_.map);
    row = rig_metrics::Row();
    row.n = r[0];
    row.diff_sum = double(r[1]) / 3.0 / 255.0;
    row.lum_ours = 0.2126 * double(r[2]) + 0.7152 * double(r[3]) + 0.0722 * double(r[4]);
    row.lum_ref = 0.2126 * double(r[5]) + 0.7152 * double(r[6]) + 0.0722 * double(r[7]);
    row.washed = r[8];
    return true;
  }

 private:
  struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr; };
  bool Fail(const char* w) { why_ = w; ready_ = false; return false; }
  bool MakeBuffer(size_t bytes, Buf& b) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes, bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bi, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev_, b.buf, &req);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int type = -1;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & want) == want) { type = int(i); break; }
    if (type < 0) return false;
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = req.size, mi.memoryTypeIndex = uint32_t(type);
    if (vkAllocateMemory(dev_, &mi, nullptr, &b.mem) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(dev_, b.buf, b.mem, 0) != VK_SUCCESS) return false;
    return vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map) == VK_SUCCESS;
  }
  void Free(Buf& b) {
    if (b.map) vkUnmapMemory(dev_, b.mem);
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
    b = Buf();
  }
  void Destroy() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    if (fence_) vkDestroyFence(dev_, fence_, nullptr);
    if (cmd_pool_) vkDestroyCommandPool(dev_, cmd_pool_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev_, pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(dev_, pipeline_, nullptr);
    if (module_) vkDestroyShaderModule(dev_, module_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    Free(ours_), Free(ref_), Free(mask_), Free(out_);
    fence_ = VK_NULL_HANDLE, cmd_pool_ = VK_NULL_HANDLE, pool_ = VK_NULL_HANDLE, pipeline_ = VK_NULL_HANDLE, module_ = VK_NULL_HANDLE, layout_ = VK_NULL_HANDLE, set_layout_ = VK_NULL_HANDLE;
    dev_ = VK_NULL_HANDLE, ready_ = false;
  }
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice dev_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  size_t px_ = 0;
  VkPhysicalDeviceMemoryProperties mem_props_{};
  Buf ours_, ref_, mask_, out_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkShaderModule module_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool ready_ = false;
  std::string why_;
};

}  // namespace rig_metrics_gpu
