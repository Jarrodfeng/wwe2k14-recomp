// Session 95: the Numbers capture on a real GPU, with no game: our own test shader, patched by diag_spirv.cpp, drawn as the diagnostic run draws it
// (point list, the two uint targets, the same render pass shape), read back, compared bit for bit, under the validation layer if it is there.
//   diag_capture_gpu_test VERTEX.spv
// Built and run by tools\native\diag_spirv_test.py --gpu. Nothing here comes from the game.
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "diag_spirv.h"

using namespace rex::graphics::vulkan::diag;

static int g_validation_errors = 0;
static VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    ++g_validation_errors;
    std::printf("validation: %s\n", data->pMessage);
  }
  return VK_FALSE;
}

#define CK(x)                                       \
  do {                                              \
    VkResult r_ = (x);                              \
    if (r_ != VK_SUCCESS) {                         \
      std::printf("%s failed: %d\n", #x, int(r_));  \
      return 2;                                     \
    }                                               \
  } while (0)

static uint32_t MemType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties m;
  vkGetPhysicalDeviceMemoryProperties(pd, &m);
  for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  return 0;
}

static std::vector<uint32_t> Load(const char* path) {
  std::vector<uint32_t> w;
  FILE* f = std::fopen(path, "rb");
  if (!f) return w;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  w.resize(size_t(n) / 4);
  if (std::fread(w.data(), 4, w.size(), f) != w.size()) w.clear();
  std::fclose(f);
  return w;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::vector<uint32_t> vs_words = Load(argv[1]);
  PositionPatch patch;
  std::string why;
  if (vs_words.empty() || !PatchPositionCapture(vs_words.data(), vs_words.size(), 16, patch, &why)) {
    std::printf("patch failed: %s\n", why.c_str());
    return 2;
  }
  const std::vector<uint32_t> fs_words = CaptureFragmentModule(patch.location);

  uint32_t layer_count = 0;
  vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
  std::vector<VkLayerProperties> layers(layer_count);
  vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
  bool have_validation = false;
  for (const auto& l : layers) have_validation |= !std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
  const char* layer = "VK_LAYER_KHRONOS_validation";
  const char* ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledLayerCount = have_validation ? 1 : 0;
  ici.ppEnabledLayerNames = &layer;
  ici.enabledExtensionCount = have_validation ? 1 : 0;
  ici.ppEnabledExtensionNames = &ext;
  VkInstance instance;
  CK(vkCreateInstance(&ici, nullptr, &instance));
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  if (have_validation) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    mi.pfnUserCallback = OnMessage;
    if (create) create(instance, &mi, nullptr, &messenger);
  }
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  if (devices.empty()) return 2;
  const VkPhysicalDevice pd = devices[0];
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  std::printf("GPU: %s, validation layer: %s\n", props.deviceName, have_validation ? "on" : "not installed");
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures features{};
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.pEnabledFeatures = &features;  // no vertexPipelineStoresAndAtomics: the capture must not need it
  VkDevice dev;
  CK(vkCreateDevice(pd, &di, nullptr, &dev));
  VkQueue queue;
  vkGetDeviceQueue(dev, 0, 0, &queue);

  const auto make_image = [&](VkFormat format, VkImage& image) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {kCaptureSize, kCaptureSize, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vkCreateImage(dev, &ii, nullptr, &image);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory memory;
    vkAllocateMemory(dev, &ai, nullptr, &memory);
    vkBindImageMemory(dev, image, memory, 0);
  };
  const auto make_view = [&](VkImage image, VkFormat format) {
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = format;
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    vkCreateImageView(dev, &v, nullptr, &view);
    return view;
  };
  const auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, void** map) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    vkCreateBuffer(dev, &bi, nullptr, &buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory;
    vkAllocateMemory(dev, &ai, nullptr, &memory);
    vkBindBufferMemory(dev, buffer, memory, 0);
    vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, map);
  };
  VkImage pos, ids;
  make_image(VK_FORMAT_R32G32B32A32_UINT, pos);
  make_image(VK_FORMAT_R32_UINT, ids);
  const VkImageView views[2] = {make_view(pos, VK_FORMAT_R32G32B32A32_UINT), make_view(ids, VK_FORMAT_R32_UINT)};

  // the render pass as MakePass in diag_layer.cpp
  VkAttachmentDescription a[2] = {};
  a[0].format = VK_FORMAT_R32G32B32A32_UINT;
  a[1].format = VK_FORMAT_R32_UINT;
  for (auto& d : a) {
    d.samples = VK_SAMPLE_COUNT_1_BIT;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    d.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  }
  const VkAttachmentReference refs[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 2;
  sub.pColorAttachments = refs;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 2;
  rp.pAttachments = a;
  rp.subpassCount = 1;
  rp.pSubpasses = &sub;
  VkRenderPass pass;
  CK(vkCreateRenderPass(dev, &rp, nullptr, &pass));
  VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fi.renderPass = pass;
  fi.attachmentCount = 2;
  fi.pAttachments = views;
  fi.width = fi.height = kCaptureSize;
  fi.layers = 1;
  VkFramebuffer fb;
  CK(vkCreateFramebuffer(dev, &fi, nullptr, &fb));

  const auto module = [&](const std::vector<uint32_t>& w) {
    VkShaderModuleCreateInfo c{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    c.codeSize = w.size() * 4;
    c.pCode = w.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &c, nullptr, &m);
    return m;
  };
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = module(patch.words);
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = module(fs_words);
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend[2] = {};
  blend[0].colorWriteMask = blend[1].colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 2;
  cb.pAttachments = blend;
  const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dynamics;
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkPipelineLayout layout;
  CK(vkCreatePipelineLayout(dev, &li, nullptr, &layout));
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.stageCount = 2;
  gi.pStages = stages;
  gi.pVertexInputState = &vi;
  gi.pInputAssemblyState = &ia;
  gi.pViewportState = &vp;
  gi.pRasterizationState = &rs;
  gi.pMultisampleState = &ms;
  gi.pColorBlendState = &cb;
  gi.pDynamicState = &dy;
  gi.layout = layout;
  gi.renderPass = pass;
  VkPipeline pipeline;
  CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &pipeline));

  // Indices as a big-endian guest buffer reaches the shader: 32-bit, byte-swapped.
  constexpr uint32_t kVertices = 30000;
  VkBuffer index_buffer, read_buffer;
  void *index_map, *read_map;
  make_buffer(kVertices * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, &index_map);
  make_buffer(VkDeviceSize(kCaptureSize) * kCaptureSize * 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, read_buffer, &read_map);
  std::vector<uint32_t> raws(kVertices);
  for (uint32_t i = 0; i < kVertices; ++i) {
    raws[i] = ((i & 0xFFu) << 24) | ((i & 0xFF00u) << 8);
    static_cast<uint32_t*>(index_map)[i] = raws[i];
  }
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  VkCommandPool cmd_pool;
  CK(vkCreateCommandPool(dev, &pi, nullptr, &cmd_pool));
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = cmd_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer c;
  CK(vkAllocateCommandBuffers(dev, &ai, &c));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CK(vkBeginCommandBuffer(c, &bi));
  VkClearValue clear[2];
  std::memset(clear, 0xFF, sizeof(clear));
  VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = pass;
  rb.framebuffer = fb;
  rb.renderArea.extent = {kCaptureSize, kCaptureSize};
  rb.clearValueCount = 2;
  rb.pClearValues = clear;
  vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const VkViewport view{0.0f, 0.0f, float(kCaptureSize), float(kCaptureSize), 0.0f, 1.0f};
  const VkRect2D scissor{{0, 0}, {kCaptureSize, kCaptureSize}};
  vkCmdSetViewport(c, 0, 1, &view);
  vkCmdSetScissor(c, 0, 1, &scissor);
  vkCmdBindIndexBuffer(c, index_buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(c, kVertices, 1, 0, 0, 0);
  vkCmdEndRenderPass(c);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kCaptureSize, kCaptureSize, 1};
  vkCmdCopyImageToBuffer(c, pos, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buffer, 1, &region);
  region.bufferOffset = VkDeviceSize(kCaptureSize) * kCaptureSize * 16;
  vkCmdCopyImageToBuffer(c, ids, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buffer, 1, &region);
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  CK(vkEndCommandBuffer(c));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &c;
  CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  CK(vkQueueWaitIdle(queue));

  const uint32_t* out_pos = static_cast<const uint32_t*>(read_map);
  const uint32_t* out_id = out_pos + size_t(kCaptureSize) * kCaptureSize * 4;
  uint32_t found = 0, wrong_id = 0, wrong_bits = 0, nan_kept = 0, nan_lost = 0, denormal_kept = 0, written = 0;
  for (uint32_t i = 0; i < kCaptureSize * kCaptureSize; ++i) written += out_id[i] != 0xFFFFFFFFu;
  for (uint32_t i = 0; i < kVertices; ++i) {
    const uint32_t raw = raws[i], slot = CaptureSlot(raw);
    if (out_id[slot] != raw) {
      ++wrong_id;
      continue;
    }
    ++found;
    // what the test shader computes (tools\native\diag_spirv_test.py GPU_SHADER): a position from the index, bits as bits
    const uint32_t expected[4] = {raw, raw ^ 0x3F800000u, 0x7FC00000u | (i & 7u), 1u};  // x: any bits, y: any bits, z: NaN, w: a denormal
    const uint32_t* got = out_pos + size_t(slot) * 4;
    for (int k = 0; k < 4; ++k) {
      const bool is_nan = (expected[k] & 0x7F800000u) == 0x7F800000u && (expected[k] & 0x7FFFFFu);
      if (got[k] == expected[k]) {
        nan_kept += k == 2;
        denormal_kept += k == 3;
      } else if (is_nan && (got[k] & 0x7F800000u) == 0x7F800000u && (got[k] & 0x7FFFFFu)) {
        ++nan_lost;  // still a NaN, payload changed
      } else {
        ++wrong_bits;
      }
    }
  }
  std::printf("written %u pixels of %u vertices; found %u, wrong id %u, wrong bits %u, NaN exact %u / NaN with another payload %u, denormal exact %u; validation errors %d\n", written, kVertices, found, wrong_id,
              wrong_bits, nan_kept, nan_lost, denormal_kept, g_validation_errors);
  const bool pass_all = found == kVertices && !wrong_id && !wrong_bits && !g_validation_errors;
  std::printf("diag_capture_gpu_test: %s\n", pass_all ? "PASS" : "FAIL");
  return pass_all ? 0 : 1;
}
