// Standalone Vulkan compute runner for offline pass tests (Session 82, harness builder). No game, no SDK: Vulkan headers and
// loader from the Vulkan SDK (C:\VulkanSDK), built by build.ps1 with the repo's clang. Used by compute_test.py.
//
//   run_compute.exe --spv shader.spv [--entry main] --groups X Y Z [--push file.bin]
//       --bind BINDING:KIND:FORMAT:W:H:INFILE ...   (BINDING is N = set 0, or S/N = descriptor set S binding N; Session 93)
//       --out  BINDING:OUTFILE ...                  (read back after the dispatch: kinds img and ssbo)
//   KIND   tex   combined image sampler (nearest, clamp to edge), read only
//          img   storage image (read / write, layout GENERAL)
//          ssbo  storage buffer  (W = size in bytes, H ignored)
//          ubo   uniform buffer  (W = size in bytes)
//   FORMAT rgba8 bgra8 r8 rg8 r16f rg16f rgba16f r32f rg32f rgba32f r32u rgba16s   (ignored for ssbo / ubo)
//   INFILE raw bytes, row pitch = W * bytes per pixel, rows top to bottom; "-" = zeros.
// Prints the GPU name, the dispatch time in microseconds and "OK"; exit code 0 on success.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#define VK(x)                                                                        \
  do {                                                                               \
    VkResult r_ = (x);                                                               \
    if (r_ != VK_SUCCESS) {                                                          \
      fprintf(stderr, "run_compute: %s failed: %d (line %d)\n", #x, int(r_), __LINE__); \
      exit(2);                                                                       \
    }                                                                                \
  } while (0)

struct Fmt { const char* name; VkFormat fmt; uint32_t bpp; };
static const Fmt kFormats[] = {
    {"rgba8", VK_FORMAT_R8G8B8A8_UNORM, 4}, {"bgra8", VK_FORMAT_B8G8R8A8_UNORM, 4}, {"r8", VK_FORMAT_R8_UNORM, 1},
    {"rg8", VK_FORMAT_R8G8_UNORM, 2},       {"r16f", VK_FORMAT_R16_SFLOAT, 2},      {"rg16f", VK_FORMAT_R16G16_SFLOAT, 4},
    {"rgba16f", VK_FORMAT_R16G16B16A16_SFLOAT, 8}, {"r32f", VK_FORMAT_R32_SFLOAT, 4}, {"rg32f", VK_FORMAT_R32G32_SFLOAT, 8},
    {"rgba32f", VK_FORMAT_R32G32B32A32_SFLOAT, 16}, {"r32u", VK_FORMAT_R32_UINT, 4}, {"rgba16s", VK_FORMAT_R16G16B16A16_SNORM, 8},
};

struct Binding {
  uint32_t index = 0;
  uint32_t set = 0;
  std::string kind, infile, outfile;
  const Fmt* fmt = nullptr;
  uint32_t w = 0, h = 0;
  uint64_t bytes = 0;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkBuffer buf = VK_NULL_HANDLE;       // ssbo / ubo (device local)
  VkDeviceMemory buf_mem = VK_NULL_HANDLE;
  VkImage img = VK_NULL_HANDLE;
  VkDeviceMemory img_mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

static VkPhysicalDevice g_pd;
static VkDevice g_dev;

static uint32_t FindMemory(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  fprintf(stderr, "run_compute: no memory type\n");
  exit(2);
}

static void MakeBuffer(uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer* b, VkDeviceMemory* m) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  VK(vkCreateBuffer(g_dev, &bi, nullptr, b));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(g_dev, *b, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = FindMemory(mr.memoryTypeBits, props);
  VK(vkAllocateMemory(g_dev, &ai, nullptr, m));
  VK(vkBindBufferMemory(g_dev, *b, *m, 0));
}

static std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    fprintf(stderr, "run_compute: cannot read %s\n", path.c_str());
    exit(2);
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  std::string spv, entry = "main", push_file;
  uint32_t groups[3] = {1, 1, 1};
  std::vector<Binding> binds;
  std::map<uint32_t, std::string> outs;  // key = set * 1000 + binding
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--spv" && i + 1 < argc) spv = argv[++i];
    else if (a == "--entry" && i + 1 < argc) entry = argv[++i];
    else if (a == "--push" && i + 1 < argc) push_file = argv[++i];
    else if (a == "--groups" && i + 3 < argc) { for (int k = 0; k < 3; ++k) groups[k] = uint32_t(atoi(argv[++i])); }
    else if (a == "--bind" && i + 1 < argc) {
      std::string s = argv[++i];
      std::vector<std::string> p;
      size_t pos = 0;
      while (true) {  // split on ':' - but a Windows path has one, so the file is everything after the 5th ':'
        if (p.size() == 5) { p.push_back(s.substr(pos)); break; }
        size_t q = s.find(':', pos);
        if (q == std::string::npos) { p.push_back(s.substr(pos)); break; }
        p.push_back(s.substr(pos, q - pos));
        pos = q + 1;
      }
      if (p.size() != 6) { fprintf(stderr, "run_compute: bad --bind %s\n", s.c_str()); return 2; }
      Binding b;
      {
        size_t sl = p[0].find('/');
        if (sl != std::string::npos) { b.set = uint32_t(atoi(p[0].substr(0, sl).c_str())); b.index = uint32_t(atoi(p[0].substr(sl + 1).c_str())); }
        else b.index = uint32_t(atoi(p[0].c_str()));
      }
      b.kind = p[1];
      for (const Fmt& f : kFormats) if (p[2] == f.name) b.fmt = &f;
      b.w = uint32_t(atoi(p[3].c_str()));
      b.h = uint32_t(atoi(p[4].c_str()));
      b.infile = p[5];
      if ((b.kind == "tex" || b.kind == "img") && !b.fmt) { fprintf(stderr, "run_compute: unknown format %s\n", p[2].c_str()); return 2; }
      b.bytes = (b.kind == "ssbo" || b.kind == "ubo") ? b.w : uint64_t(b.w) * b.h * (b.fmt ? b.fmt->bpp : 0);
      binds.push_back(b);
    } else if (a == "--out" && i + 1 < argc) {
      std::string s = argv[++i];
      size_t q = s.find(':');
      std::string k = s.substr(0, q);
      size_t sl = k.find('/');
      uint32_t key = sl == std::string::npos ? uint32_t(atoi(k.c_str())) : uint32_t(atoi(k.substr(0, sl).c_str())) * 1000 + uint32_t(atoi(k.substr(sl + 1).c_str()));
      outs[key] = s.substr(q + 1);
    } else { fprintf(stderr, "run_compute: unknown argument %s\n", a.c_str()); return 2; }
  }
  if (spv.empty()) { fprintf(stderr, "run_compute: --spv is required\n"); return 2; }
  std::vector<uint8_t> code = ReadFile(spv);
  std::vector<uint8_t> push;
  if (!push_file.empty()) push = ReadFile(push_file);

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance inst;
  VK(vkCreateInstance(&ici, nullptr, &inst));
  uint32_t n = 0;
  VK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
  std::vector<VkPhysicalDevice> pds(n);
  VK(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
  g_pd = pds[0];
  for (auto pd : pds) {
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(pd, &pr);
    if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_pd = pd; break; }
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(g_pd, &props);
  printf("GPU: %s\n", props.deviceName);
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qn, qf.data());
  uint32_t family = 0;
  for (uint32_t i = 0; i < qn; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkPhysicalDeviceFeatures feats{};
  feats.shaderStorageImageExtendedFormats = VK_TRUE;
  feats.shaderStorageImageReadWithoutFormat = VK_TRUE;
  feats.shaderStorageImageWriteWithoutFormat = VK_TRUE;
  feats.shaderInt16 = VK_FALSE;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.pEnabledFeatures = &feats;
  VK(vkCreateDevice(g_pd, &dci, nullptr, &g_dev));
  VkQueue queue;
  vkGetDeviceQueue(g_dev, family, 0, &queue);
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = family;
  VkCommandPool pool;
  VK(vkCreateCommandPool(g_dev, &cpi, nullptr, &pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cb;
  VK(vkAllocateCommandBuffers(g_dev, &cai, &cb));

  // Resources.
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler;
  VK(vkCreateSampler(g_dev, &si, nullptr, &sampler));
  for (Binding& b : binds) {
    if (!b.bytes) { fprintf(stderr, "run_compute: binding %u has no size\n", b.index); return 2; }
    MakeBuffer(b.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &b.staging, &b.staging_mem);
    VK(vkMapMemory(g_dev, b.staging_mem, 0, b.bytes, 0, &b.mapped));
    memset(b.mapped, 0, size_t(b.bytes));
    if (b.infile != "-") {
      std::vector<uint8_t> d = ReadFile(b.infile);
      if (d.size() != b.bytes) { fprintf(stderr, "run_compute: %s is %zu bytes, binding %u needs %llu\n", b.infile.c_str(), d.size(), b.index, (unsigned long long)b.bytes); return 2; }
      memcpy(b.mapped, d.data(), size_t(b.bytes));
    }
    if (b.kind == "ssbo" || b.kind == "ubo") {
      VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             (b.kind == "ssbo" ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      MakeBuffer(b.bytes, u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &b.buf, &b.buf_mem);
    } else {
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ii.imageType = VK_IMAGE_TYPE_2D;
      ii.format = b.fmt->fmt;
      ii.extent = {b.w, b.h, 1};
      ii.mipLevels = ii.arrayLayers = 1;
      ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.tiling = VK_IMAGE_TILING_OPTIMAL;
      ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (b.kind == "tex" ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT);
      ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK(vkCreateImage(g_dev, &ii, nullptr, &b.img));
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(g_dev, b.img, &mr);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = mr.size;
      ai.memoryTypeIndex = FindMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VK(vkAllocateMemory(g_dev, &ai, nullptr, &b.img_mem));
      VK(vkBindImageMemory(g_dev, b.img, b.img_mem, 0));
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = b.img;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = b.fmt->fmt;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK(vkCreateImageView(g_dev, &vi, nullptr, &b.view));
    }
  }

  // Layout, pipeline, descriptors (one layout / set per descriptor set number used; Session 93: more than set 0).
  uint32_t set_count = 1;
  for (Binding& b : binds) set_count = std::max(set_count, b.set + 1);
  auto dtype = [](const Binding& b) {
    return b.kind == "tex" ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
         : b.kind == "img" ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
         : b.kind == "ssbo" ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  };
  std::vector<VkDescriptorSetLayout> dsls(set_count);
  std::map<VkDescriptorType, uint32_t> counts;
  for (uint32_t si_ = 0; si_ < set_count; ++si_) {
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (Binding& b : binds) {
      if (b.set != si_) continue;
      VkDescriptorSetLayoutBinding x{};
      x.binding = b.index;
      x.descriptorCount = 1;
      x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      x.descriptorType = dtype(b);
      counts[x.descriptorType]++;
      lb.push_back(x);
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = uint32_t(lb.size());
    dli.pBindings = lb.data();
    VK(vkCreateDescriptorSetLayout(g_dev, &dli, nullptr, &dsls[si_]));
  }
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, uint32_t((push.size() + 3) & ~size_t(3))};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = set_count;
  pli.pSetLayouts = dsls.data();
  if (!push.empty()) { pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr; }
  VkPipelineLayout pl;
  VK(vkCreatePipelineLayout(g_dev, &pli, nullptr, &pl));
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = code.size();
  smi.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule sm;
  VK(vkCreateShaderModule(g_dev, &smi, nullptr, &sm));
  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, entry.c_str(), nullptr};
  cpci.layout = pl;
  VkPipeline pipe;
  VK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
  std::vector<VkDescriptorPoolSize> ps;
  for (auto& kv : counts) ps.push_back({kv.first, kv.second});
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = set_count;
  dpi.poolSizeCount = uint32_t(ps.size());
  dpi.pPoolSizes = ps.data();
  VkDescriptorPool dp;
  VK(vkCreateDescriptorPool(g_dev, &dpi, nullptr, &dp));
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = dp;
  dai.descriptorSetCount = set_count;
  dai.pSetLayouts = dsls.data();
  std::vector<VkDescriptorSet> dss(set_count);
  VK(vkAllocateDescriptorSets(g_dev, &dai, dss.data()));
  std::vector<VkDescriptorImageInfo> ii(binds.size());
  std::vector<VkDescriptorBufferInfo> bi(binds.size());
  std::vector<VkWriteDescriptorSet> wr;
  for (size_t i = 0; i < binds.size(); ++i) {
    Binding& b = binds[i];
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = dss[b.set];
    w.dstBinding = b.index;
    w.descriptorCount = 1;
    w.descriptorType = dtype(b);
    if (b.kind == "tex") { ii[i] = {sampler, b.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; w.pImageInfo = &ii[i]; }
    else if (b.kind == "img") { ii[i] = {VK_NULL_HANDLE, b.view, VK_IMAGE_LAYOUT_GENERAL}; w.pImageInfo = &ii[i]; }
    else { bi[i] = {b.buf, 0, b.bytes}; w.pBufferInfo = &bi[i]; }
    wr.push_back(w);
  }
  vkUpdateDescriptorSets(g_dev, uint32_t(wr.size()), wr.data(), 0, nullptr);

  // Record: upload, dispatch (timed), read back.
  VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpi.queryCount = 2;
  VkQueryPool qp;
  VK(vkCreateQueryPool(g_dev, &qpi, nullptr, &qp));
  VkCommandBufferBeginInfo bgi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK(vkBeginCommandBuffer(cb, &bgi));
  auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                     VkPipelineStageFlags ss, VkPipelineStageFlags ds2) {
    VkImageMemoryBarrier m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    m.srcAccessMask = sa; m.dstAccessMask = da; m.oldLayout = from; m.newLayout = to;
    m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    m.image = img; m.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, ss, ds2, 0, 0, nullptr, 0, nullptr, 1, &m);
  };
  for (Binding& b : binds) {
    if (b.buf) {
      VkBufferCopy c{0, 0, b.bytes};
      vkCmdCopyBuffer(cb, b.staging, b.buf, 1, &c);
    } else {
      barrier(b.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy c{};
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.imageExtent = {b.w, b.h, 1};
      vkCmdCopyBufferToImage(cb, b.staging, b.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
      VkImageLayout to = b.kind == "tex" ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
      barrier(b.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, to, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
  }
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  vkCmdResetQueryPool(cb, qp, 0, 2);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, set_count, dss.data(), 0, nullptr);
  if (!push.empty()) {
    std::vector<uint8_t> padded(pcr.size, 0);
    memcpy(padded.data(), push.data(), push.size());
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcr.size, padded.data());
  }
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
  vkCmdDispatch(cb, groups[0], groups[1], groups[2]);
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
  VkMemoryBarrier mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr);
  for (Binding& b : binds) {
    if (!outs.count(b.set * 1000 + b.index) || b.kind == "tex" || b.kind == "ubo") continue;
    if (b.buf) {
      VkBufferCopy c{0, 0, b.bytes};
      vkCmdCopyBuffer(cb, b.buf, b.staging, 1, &c);
    } else {
      VkBufferImageCopy c{};
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.imageExtent = {b.w, b.h, 1};
      vkCmdCopyImageToBuffer(cb, b.img, VK_IMAGE_LAYOUT_GENERAL, b.staging, 1, &c);
    }
  }
  VkMemoryBarrier mb3{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb3, 0, nullptr, 0, nullptr);
  VK(vkEndCommandBuffer(cb));
  VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  sub.commandBufferCount = 1;
  sub.pCommandBuffers = &cb;
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  VK(vkCreateFence(g_dev, &fi, nullptr, &fence));
  VK(vkQueueSubmit(queue, 1, &sub, fence));
  VK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, 60ull * 1000000000ull));
  uint64_t ts[2] = {0, 0};
  vkGetQueryPoolResults(g_dev, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  printf("dispatch %u x %u x %u: %.1f us\n", groups[0], groups[1], groups[2], double(ts[1] - ts[0]) * props.limits.timestampPeriod / 1000.0);
  for (Binding& b : binds) {
    auto it = outs.find(b.set * 1000 + b.index);
    if (it == outs.end()) continue;
    if (b.kind == "tex" || b.kind == "ubo") { fprintf(stderr, "run_compute: binding %u (%s) cannot be read back\n", b.index, b.kind.c_str()); return 2; }
    std::ofstream f(it->second, std::ios::binary);
    f.write(static_cast<const char*>(b.mapped), std::streamsize(b.bytes));
    printf("wrote %s (%llu bytes)\n", it->second.c_str(), (unsigned long long)b.bytes);
  }
  printf("OK\n");
  return 0;
}
