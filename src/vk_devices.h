// Session 37: the Vulkan graphics devices of this PC, for the game (src\gpu_pick.h: which one, and which render path
// it can run) and the launcher (integrated graphics -> the light renderer; where FSR makes sense). Header-only, no
// ReXGlue dependency (the launcher includes it too).
//
// interlock: the device has what the SDK's accurate render target path (render_target_path_vulkan=fsi) needs:
// fragment shader pixel or sample interlock, fragment stores and atomics, sample rate shading and standard sample
// locations (thirdparty\rexglue-sdk-src\src\graphics\vulkan\render_target_cache.cpp, Initialize). Without it the SDK
// falls back to the host render target path (fbo) by itself. AMD's Windows driver has not exposed interlock (AMDVLK
// issue 108; unconfirmed claims that a 2024 driver added it); Intel and NVIDIA do; Mesa's RADV (Linux, Steam Deck) since
// Mesa 23.2. The SDK also needs a storage buffer slot count this list does not check (all known interlock devices have
// enough).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wwe2k14::vkdev {

struct Device {
  std::string name;
  uint32_t vendor = 0;  // 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
  uint32_t device_id = 0;
  VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
  uint64_t local_mb = 0;  // device-local memory heaps (an integrated GPU's share of system memory)
  uint32_t api_version = 0, driver_version = 0;
  bool interlock = false;
  bool integrated() const { return type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; }
  bool discrete() const { return type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU; }
  const char* type_text() const {
    return discrete() ? "discrete" : integrated() ? "integrated" : type == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu" : "other";
  }
};

inline std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// In the loader's order (the SDK's vulkan_device index). Empty when there is no Vulkan.
inline std::vector<Device> List() {
  std::vector<Device> out;
  const HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
  if (!lib) return out;
  const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
  const auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
  VkInstance inst = VK_NULL_HANDLE;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "wwe2k14 device list";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  if (!create || create(&ci, nullptr, &inst) != VK_SUCCESS) {
    FreeLibrary(lib);
    return out;
  }
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
  const auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
  const auto mem =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
  const auto features2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(inst, "vkGetPhysicalDeviceFeatures2"));
  const auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
      gipa(inst, "vkEnumerateDeviceExtensionProperties"));
  const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
  std::vector<VkPhysicalDevice> devs;
  uint32_t n = 0;
  if (enumerate && enumerate(inst, &n, nullptr) == VK_SUCCESS && n) {
    devs.resize(n);
    if (enumerate(inst, &n, devs.data()) < VK_SUCCESS) n = 0;
    devs.resize(n);
  }
  for (VkPhysicalDevice pd : devs) {
    if (!props || !mem) break;
    Device d;
    VkPhysicalDeviceProperties p{};
    props(pd, &p);
    d.name = p.deviceName, d.vendor = p.vendorID, d.device_id = p.deviceID, d.type = p.deviceType;
    d.api_version = p.apiVersion, d.driver_version = p.driverVersion;
    VkPhysicalDeviceMemoryProperties m{};
    mem(pd, &m);
    uint64_t local = 0;
    for (uint32_t h = 0; h < m.memoryHeapCount; ++h)
      if (m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local += m.memoryHeaps[h].size;
    d.local_mb = local >> 20;
    bool has_ext = false;
    uint32_t en = 0;
    if (extensions && extensions(pd, nullptr, &en, nullptr) == VK_SUCCESS && en) {
      std::vector<VkExtensionProperties> ext(en);
      if (extensions(pd, nullptr, &en, ext.data()) >= VK_SUCCESS)
        for (uint32_t i = 0; i < en; ++i)
          if (!std::strcmp(ext[i].extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)) has_ext = true;
    }
    if (has_ext && features2 && p.apiVersion >= VK_API_VERSION_1_1) {
      VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
      VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      f2.pNext = &fsi;
      features2(pd, &f2);
      d.interlock = (fsi.fragmentShaderPixelInterlock || fsi.fragmentShaderSampleInterlock) &&
                    f2.features.fragmentStoresAndAtomics && f2.features.sampleRateShading &&
                    p.limits.standardSampleLocations;
    }
    out.push_back(d);
  }
  if (destroy) destroy(inst, nullptr);
  FreeLibrary(lib);
  return out;
}

// The device the game uses for a --wwe2k14_gpu value (part of a name, any case; empty = automatic): the first whose
// name contains it, else the discrete GPU with the most device-local memory, else the first (the SDK's own pick then
// is by features and order; with one device it is that one). -1 when there is none. Same rule as src\gpu_pick.h.
inline int Pick(const std::vector<Device>& devs, const std::string& want_name) {
  const std::string want = Lower(want_name);
  int by_name = -1, discrete = -1;
  uint64_t discrete_mb = 0;
  for (int i = 0; i < int(devs.size()); ++i) {
    if (!want.empty() && by_name < 0 && Lower(devs[i].name).find(want) != std::string::npos) by_name = i;
    if (devs[i].discrete() && (discrete < 0 || devs[i].local_mb > discrete_mb)) discrete = i, discrete_mb = devs[i].local_mb;
  }
  if (by_name >= 0) return by_name;
  if (discrete >= 0) return discrete;
  return devs.empty() ? -1 : 0;
}

}  // namespace wwe2k14::vkdev
