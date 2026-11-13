#include "vk_api.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rdr::vk {

#define RDR_DEFINE(name) PFN_##name name = nullptr;
RDR_GLOBAL_FNS(RDR_DEFINE)
RDR_INSTANCE_FNS(RDR_DEFINE)
RDR_DEVICE_FNS(RDR_DEFINE)
#undef RDR_DEFINE

namespace {
PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
PFN_vkGetInstanceProcAddr instance_proc_override = nullptr;  // S10b
void* (*loader_opener)() = nullptr;                          // lane 256
}

bool LoadVulkan() {
  if (get_instance_proc) return true;
#if defined(_WIN32)
  HMODULE lib = ::LoadLibraryA("vulkan-1.dll");
  if (!lib) return false;
  get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
#else
  void* lib = loader_opener ? loader_opener() : nullptr;  // lane 256: the bundled custom driver, when one is chosen and starts
  if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return false;
  get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
  if (instance_proc_override) get_instance_proc = instance_proc_override;  // S10b: Streamline's interposer
  if (!get_instance_proc) return false;
#define RDR_LOAD(name) name = reinterpret_cast<PFN_##name>(get_instance_proc(VK_NULL_HANDLE, #name));
  RDR_GLOBAL_FNS(RDR_LOAD)
#undef RDR_LOAD
  return vkCreateInstance != nullptr;
}

void SetInstanceProcAddrOverride(PFN_vkGetInstanceProcAddr proc) { instance_proc_override = proc; }
void SetLoaderOpener(void* (*open_library)()) { loader_opener = open_library; }

void* GetInstanceProc(VkInstance instance, const char* name) { return reinterpret_cast<void*>(get_instance_proc(instance, name)); }

void LoadInstance(VkInstance instance) {
#define RDR_LOAD(name) name = reinterpret_cast<PFN_##name>(get_instance_proc(instance, #name));
  RDR_INSTANCE_FNS(RDR_LOAD)
#undef RDR_LOAD
}

void LoadDevice(VkDevice device) {
#define RDR_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
  RDR_DEVICE_FNS(RDR_LOAD)
#undef RDR_LOAD
}

}  // namespace rdr::vk
