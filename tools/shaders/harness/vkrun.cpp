// Scratch: run one compute shader (spv) over N records: binding 0 = float input, binding 1 = vec4 output.
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
  if (argc < 5) { printf("usage: vkrun shader.spv input.bin output.bin count floatsPerRecord\n"); return 1; }
  const char* spv = argv[1]; const char* inp = argv[2]; const char* outp = argv[3];
  uint32_t count = (uint32_t)atoi(argv[4]);
  std::vector<char> code = ReadFile(spv);
  std::vector<char> input = ReadFile(inp);

  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_1;
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
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  VkDevice dev; CK(vkCreateDevice(pd, &dci, nullptr, &dev));
  VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);

  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  auto findMem = [&](uint32_t bits, VkMemoryPropertyFlags f) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    printf("no memory type\n"); exit(1); return 0u; };
  auto makeBuf = [&](VkDeviceSize size, VkBuffer& b, VkDeviceMemory& m) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CK(vkCreateBuffer(dev, &bi, nullptr, &b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CK(vkAllocateMemory(dev, &mai, nullptr, &m)); CK(vkBindBufferMemory(dev, b, m, 0)); };

  VkBuffer bin, bout; VkDeviceMemory min, mout;
  VkDeviceSize outSize = (VkDeviceSize)count * 16;
  makeBuf(input.size(), bin, min); makeBuf(outSize, bout, mout);
  void* p; CK(vkMapMemory(dev, min, 0, input.size(), 0, &p)); memcpy(p, input.data(), input.size()); vkUnmapMemory(dev, min);

  VkDescriptorSetLayoutBinding bnd[2] = {};
  for (int i = 0; i < 2; ++i) { bnd[i].binding = i; bnd[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bnd[i].descriptorCount = 1; bnd[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
  VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dsl.bindingCount = 2; dsl.pBindings = bnd;
  VkDescriptorSetLayout layout; CK(vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &layout));
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &layout;
  VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = code.size(); smi.pCode = (const uint32_t*)code.data();
  VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, nullptr, &sm));
  VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = sm; cpi.stage.pName = "main"; cpi.layout = pl;
  VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
  VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
  VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
  VkDescriptorBufferInfo bi[2] = {{bin, 0, VK_WHOLE_SIZE}, {bout, 0, VK_WHOLE_SIZE}};
  VkWriteDescriptorSet wr[2] = {};
  for (int i = 0; i < 2; ++i) { wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds; wr[i].dstBinding = i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &bi[i]; }
  vkUpdateDescriptorSets(dev, 2, wr, 0, nullptr);

  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = qf;
  VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, nullptr, &cp));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
  VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
  VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CK(vkBeginCommandBuffer(cb, &cbbi));
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
  vkCmdDispatch(cb, (count + 63) / 64, 1, 1);
  CK(vkEndCommandBuffer(cb));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));

  CK(vkMapMemory(dev, mout, 0, outSize, 0, &p));
  FILE* f = fopen(outp, "wb"); fwrite(p, 1, outSize, f); fclose(f);
  vkUnmapMemory(dev, mout);
  printf("done %u records\n", count);
  return 0;
}
