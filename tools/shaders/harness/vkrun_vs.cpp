// Session 87 (stage 4): the vertex shader arithmetic harness's runner. Scratch tool, our own code, no game data.
// Runs one compute shader (made by vs_harness.py from a vertex shader, ours or the translation's) over rounds x verts
// invocations. Per round the input file holds: the system constants (512 bytes), the float constants (16384: 1024 vec4, the first 256 declared by a skinned shader), the fetch
// constants (768), the bool / loop constants (256), the shared memory (verts x 12 dwords at dword 0); the shader's
// bindings (all in set 0): 0 shared memory, 1 system constants, 2 float constants, 3 bool / loop constants, 4 fetch
// constants, 5 output (verts x 13 vec4: position, interpolators 0-11 per round).
// usage: vkrun_vs shader.spv input.bin output.bin rounds verts
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("Vulkan error %d at line %d: %s\n", (int)r_, __LINE__, #x); exit(1); } } while (0)

static std::vector<char> ReadFile(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) { printf("cannot open %s\n", p); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  std::vector<char> v(n); fread(v.data(), 1, n, f); fclose(f); return v;
}

int main(int argc, char** argv) {
  if (argc < 6) { printf("usage: vkrun_vs shader.spv input.bin output.bin rounds verts\n"); return 1; }
  const uint32_t rounds = (uint32_t)atoi(argv[4]), verts = (uint32_t)atoi(argv[5]);
  std::vector<char> code = ReadFile(argv[1]);
  std::vector<char> input = ReadFile(argv[2]);
  const size_t kSys = 512, kFlt = 16384, kFetch = 768, kBool = 256, kVerts = (size_t)verts * 48;
  const size_t perRound = kSys + kFlt + kFetch + kBool + kVerts;
  if (input.size() != perRound * rounds) { printf("input size %zu != %zu\n", input.size(), perRound * rounds); return 1; }

  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
  VkInstance inst; CK(vkCreateInstance(&ici, nullptr, &inst));
  uint32_t pdn = 0; vkEnumeratePhysicalDevices(inst, &pdn, nullptr);
  std::vector<VkPhysicalDevice> pds(pdn); vkEnumeratePhysicalDevices(inst, &pdn, pds.data());
  VkPhysicalDevice pd = pds[0];
  for (auto d : pds) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = d; break; } }
  { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p); printf("device: %s\n", p.deviceName); }
  uint32_t qfn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfn, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qfn); vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfn, qfs.data());
  uint32_t qf = 0; for (uint32_t i = 0; i < qfn; ++i) if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
  float prio = 1.0f;
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; f12.scalarBlockLayout = VK_TRUE;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &f12; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  VkDevice dev; CK(vkCreateDevice(pd, &dci, nullptr, &dev));
  VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);

  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  auto findMem = [&](uint32_t bits, VkMemoryPropertyFlags f) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    printf("no memory type\n"); exit(1); return 0u; };
  struct Buf { VkBuffer b; VkDeviceMemory m; VkDeviceSize size; void* p; };
  auto makeBuf = [&](VkDeviceSize size) {
    Buf r; r.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CK(vkCreateBuffer(dev, &bi, nullptr, &r.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CK(vkAllocateMemory(dev, &mai, nullptr, &r.m)); CK(vkBindBufferMemory(dev, r.b, r.m, 0));
    CK(vkMapMemory(dev, r.m, 0, size, 0, &r.p));
    return r; };

  Buf shared = makeBuf(kVerts), sys = makeBuf(kSys + 16), flt = makeBuf(kFlt), bl = makeBuf(kBool), fetch = makeBuf(kFetch);
  memset(sys.p, 0, sys.size);  // Session 93: the system constants block is 516 bytes now (shared_memory_window_base_dwords at 512, 0 here); the input file's 512 bytes are copied over the start
  Buf out = makeBuf((VkDeviceSize)verts * 208);
  Buf bufs[6] = {shared, sys, flt, bl, fetch, out};

  VkDescriptorSetLayoutBinding bnd[6] = {};
  for (int i = 0; i < 6; ++i) { bnd[i].binding = i; bnd[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bnd[i].descriptorCount = 1; bnd[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
  VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dsl.bindingCount = 6; dsl.pBindings = bnd;
  VkDescriptorSetLayout layout; CK(vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &layout));
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &layout;
  VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = code.size(); smi.pCode = (const uint32_t*)code.data();
  VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, nullptr, &sm));
  VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = sm; cpi.stage.pName = "main"; cpi.layout = pl;
  VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
  VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
  VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
  VkDescriptorBufferInfo bi[6]; VkWriteDescriptorSet wr[6] = {};
  for (int i = 0; i < 6; ++i) { bi[i] = {bufs[i].b, 0, VK_WHOLE_SIZE}; wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds; wr[i].dstBinding = i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &bi[i]; }
  vkUpdateDescriptorSets(dev, 6, wr, 0, nullptr);

  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = qf;
  VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, nullptr, &cp));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
  VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
  FILE* fo = fopen(argv[3], "wb");
  for (uint32_t r = 0; r < rounds; ++r) {
    const char* in = input.data() + perRound * r;
    memcpy(sys.p, in, kSys); memcpy(flt.p, in + kSys, kFlt); memcpy(fetch.p, in + kSys + kFlt, kFetch);
    memcpy(bl.p, in + kSys + kFlt + kFetch, kBool); memcpy(shared.p, in + kSys + kFlt + kFetch + kBool, kVerts);
    CK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    vkCmdDispatch(cb, (verts + 63) / 64, 1, 1);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    fwrite(out.p, 1, (size_t)verts * 208, fo);
  }
  fclose(fo);
  printf("done %u rounds x %u vertices\n", rounds, verts);
  return 0;
}
