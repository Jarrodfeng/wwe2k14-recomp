// Session 31 (a playtester's laptop started the game on its Intel integrated GPU): which graphics card the game uses.
//
// The SDK's `vulkan_device` (an index into vkEnumeratePhysicalDevices' list; -1 = auto) auto-picks by four feature
// bits only and keeps the driver's order on a tie, so an integrated GPU listed first wins
// (thirdparty\rexglue-sdk-src\src\ui\vulkan\vulkan_provider.cpp:78-139; a forced index that fails falls back to
// auto). Before the GPU starts (OnPreSetup), when `vulkan_device` is left at its default, Choose() lists the Vulkan
// devices itself (the same loader, so the same order) and sets `vulkan_device` to:
//   --wwe2k14_gpu=NAME   the first device whose name contains NAME (any case; the launcher's chooser), else
//   the discrete GPU with the most device-local memory, else leaves auto.
// Every device and the choice go in the log ("[wwe2k14][gpu]"). The SDK's own list follows at WARN ("Available
// Vulkan physical devices"), so the index can be checked against it.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

REXCVAR_DEFINE_STRING(wwe2k14_gpu, "", "wwe2k14",
                      "Graphics card: part of its name (e.g. 'RTX 4090'). Empty = the dedicated GPU with the most memory "
                      "(Session 31: the SDK's auto choice can take an integrated GPU). Ignored when --vulkan_device is set");

namespace wwe2k14::gpu_pick {

inline std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline void Choose() {
#ifdef _WIN32
  if (rex::cvar::GetFlagSource("vulkan_device") != rex::cvar::Source::kDefault) {
    REXLOG_INFO("[wwe2k14][gpu] vulkan_device was set: left as it is");
    return;
  }
  const HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
  if (!lib) {
    REXLOG_WARN("[wwe2k14][gpu] vulkan-1.dll not found: the SDK picks the device");
    return;
  }
  const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
  const auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
  VkInstance inst = VK_NULL_HANDLE;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "wwe2k14 gpu pick";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  if (!create || create(&ci, nullptr, &inst) != VK_SUCCESS) {
    REXLOG_WARN("[wwe2k14][gpu] no Vulkan instance: the SDK picks the device");
    FreeLibrary(lib);
    return;
  }
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
  const auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
  const auto mem =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
  const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
  std::vector<VkPhysicalDevice> devs;
  uint32_t n = 0;
  if (enumerate && enumerate(inst, &n, nullptr) == VK_SUCCESS && n) {
    devs.resize(n);
    if (enumerate(inst, &n, devs.data()) < VK_SUCCESS) devs.clear();
    devs.resize(n);
  }
  const std::string want = Lower(REXCVAR_GET(wwe2k14_gpu));
  int by_name = -1, discrete = -1;
  uint64_t discrete_mb = 0;
  for (uint32_t i = 0; i < devs.size() && props && mem; ++i) {
    VkPhysicalDeviceProperties p{};
    props(devs[i], &p);
    VkPhysicalDeviceMemoryProperties m{};
    mem(devs[i], &m);
    uint64_t local = 0;
    for (uint32_t h = 0; h < m.memoryHeapCount; ++h)
      if (m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local += m.memoryHeaps[h].size;
    const uint64_t mb = local >> 20;
    const bool is_discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    REXLOG_INFO("[wwe2k14][gpu] device {}: {} ({}, {} MB device-local)", i, p.deviceName,
                is_discrete ? "discrete"
                : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                        : "other",
                mb);
    if (!want.empty() && by_name < 0 && Lower(p.deviceName).find(want) != std::string::npos) by_name = int(i);
    if (is_discrete && (discrete < 0 || mb > discrete_mb)) discrete = int(i), discrete_mb = mb;
  }
  if (destroy) destroy(inst, nullptr);
  FreeLibrary(lib);
  if (!want.empty() && by_name < 0)
    REXLOG_WARN("[wwe2k14][gpu] no device name contains '{}': using the automatic choice", REXCVAR_GET(wwe2k14_gpu));
  const int pick = by_name >= 0 ? by_name : discrete;
  if (pick < 0) {
    REXLOG_INFO("[wwe2k14][gpu] no discrete GPU: the SDK picks the device");
    return;
  }
  rex::cvar::SetFlagByName("vulkan_device", std::to_string(pick));
  REXLOG_INFO("[wwe2k14][gpu] vulkan_device = {} ({})", pick, by_name >= 0 ? "--wwe2k14_gpu" : "the discrete GPU with the most memory");
#endif
}

}  // namespace wwe2k14::gpu_pick
