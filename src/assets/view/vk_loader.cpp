// See vk_loader.h.
#include "vk_loader.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vkfn {

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define VKV_DEF(n) PFN_##n n = nullptr;
VKV_GLOBAL_FNS(VKV_DEF)
VKV_INSTANCE_FNS(VKV_DEF)
VKV_DEVICE_FNS(VKV_DEF)
#undef VKV_DEF

bool LoadLoader(std::string& err) {
  if (vkGetInstanceProcAddr) return true;
#ifdef _WIN32
  HMODULE m = ::LoadLibraryA("vulkan-1.dll");
  if (!m) { err = "vulkan-1.dll not found (no Vulkan driver)"; return false; }
  vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(m, "vkGetInstanceProcAddr");
#else
  void* m = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!m) m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!m) { err = "libvulkan.so not found"; return false; }
  vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(m, "vkGetInstanceProcAddr");
#endif
  if (!vkGetInstanceProcAddr) { err = "vkGetInstanceProcAddr missing"; return false; }
#define VKV_LOAD(n) n = (PFN_##n)vkGetInstanceProcAddr(nullptr, #n);
  VKV_GLOBAL_FNS(VKV_LOAD)
#undef VKV_LOAD
  if (!vkCreateInstance) { err = "vkCreateInstance missing"; return false; }
  return true;
}

void LoadInstance(VkInstance instance) {
#define VKV_LOAD(n) n = (PFN_##n)vkGetInstanceProcAddr(instance, #n);
  VKV_INSTANCE_FNS(VKV_LOAD)
#undef VKV_LOAD
}

void LoadDevice(VkDevice device) {
#define VKV_LOAD(n) n = (PFN_##n)vkGetDeviceProcAddr(device, #n);
  VKV_DEVICE_FNS(VKV_LOAD)
#undef VKV_LOAD
}

PFN_vkVoidFunction GetProc(const char* name, void* instance) {
  return vkGetInstanceProcAddr ? vkGetInstanceProcAddr((VkInstance)instance, name) : nullptr;
}

}  // namespace vkfn
