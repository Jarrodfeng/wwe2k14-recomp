#pragma once
// wwe2k14 - Session 72 part 7 (Android only; Jarrod: "we should have many drivers for all the most common android
// devices" -> bundled drivers built from source): a custom Vulkan driver chosen in the launcher.
//
// The APK carries custom drivers built from source (tools\android\drivers: Turnip, Mesa's Adreno driver) next to the
// game's libraries, plus libadrenotools' hook libraries (thirdparty\libadrenotools, BSD-2-Clause). With
// --wwe2k14_android_driver=<soname> the SDK's Vulkan instance opens libvulkan through libadrenotools, which loads that
// driver in place of the phone's (SetVulkanLoaderOverride, SDK patch). Before handing it over, the driver is test-started
// here (an instance, at least one GPU): if that fails, the phone's own driver is used and the log says why.
//
// A driver that crashes the game cannot be caught here: the launcher writes a trial file before PLAY
// (--wwe2k14_android_driver_trial=<path>) and this deletes it after kTrialFrames frames. A trial file still there at the
// next launcher start means the driver did not get that far: the launcher goes back to the phone's driver and says so.
//
//   --wwe2k14_android_driver=libvulkan_freedreno.so   the driver's file name in the app's library folder
//   --wwe2k14_android_lib_dir=<nativeLibraryDir>/     GameActivity passes it (where the hooks and drivers are)
//   --wwe2k14_android_driver_dir=<dir>/               part 8: a driver the player imported (DriverImport.java), in
//                                                     the app's internal storage; the hooks stay in lib_dir
//   --wwe2k14_android_tmp_dir=<cache>/                Android 9 (API 28) needs a writable folder for patched libraries

#if defined(__ANDROID__)

#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>

#include <rex/cvar.h>
#include <rex/logging.h>

// From the SDK's rex/ui/vulkan/instance.h (Session 72 patch), declared here: that header pulls in RenderDoc's, which the
// Android SDK install does not carry.
namespace rex::ui::vulkan {
using VulkanLoaderOverride = void* (*)();
void SetVulkanLoaderOverride(VulkanLoaderOverride override_fn);
}  // namespace rex::ui::vulkan

REXCVAR_DEFINE_STRING(wwe2k14_android_driver, "", "wwe2k14",
                      "Session 72 (Android): a bundled custom Vulkan driver's file name (e.g. libvulkan_freedreno.so); "
                      "empty = the phone's own driver (src/android_driver.h)");
REXCVAR_DEFINE_STRING(wwe2k14_android_lib_dir, "", "wwe2k14",
                      "Session 72 (Android): the app's native library folder with a trailing / (GameActivity sets it)");
REXCVAR_DEFINE_STRING(wwe2k14_android_driver_dir, "", "wwe2k14",
                      "Session 72 part 8 (Android): the folder (trailing /) of a driver the player imported in the "
                      "launcher; empty = the driver is in the library folder (bundled)");
REXCVAR_DEFINE_STRING(wwe2k14_android_tmp_dir, "", "wwe2k14",
                      "Session 72 (Android): a writable folder with a trailing / for libadrenotools on Android 9");
REXCVAR_DEFINE_STRING(wwe2k14_android_driver_trial, "", "wwe2k14",
                      "Session 72 (Android): the launcher's trial file for the chosen driver; deleted once the game has "
                      "drawn a few hundred frames with it");

namespace wwe2k14::android_driver {

inline constexpr uint32_t kTrialFrames = 600;  // ~10 s at 60 fps: past the boot logos, with real draws behind it
inline std::string g_driver_used;              // empty = the phone's driver
inline uint32_t g_frames = 0;

// Instance + at least one GPU through this libvulkan handle; logs the GPU and driver it reports.
inline bool TestStart(void* handle, const std::string& name) {
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
  if (!gipa) {
    REXLOG_WARN("[driver] {}: no vkGetInstanceProcAddr", name);
    return false;
  }
  auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  if (!create) {
    REXLOG_WARN("[driver] {}: no vkCreateInstance", name);
    return false;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "wwe2k14 driver test";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = create(&ci, nullptr, &instance);
  if (r != VK_SUCCESS || !instance) {
    REXLOG_WARN("[driver] {}: vkCreateInstance failed ({})", name, int(r));
    return false;
  }
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
  auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  uint32_t count = 0;
  bool ok = enumerate && props && enumerate(instance, &count, nullptr) == VK_SUCCESS && count > 0;
  if (ok) {
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(instance, &count, devices.data());
    VkPhysicalDeviceProperties p{};
    props(devices[0], &p);
    REXLOG_INFO("[driver] {}: {} GPU(s); first: {}, Vulkan {}.{}.{}, driver 0x{:08X}", name, count, p.deviceName,
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
  } else {
    REXLOG_WARN("[driver] {}: the driver reports no GPU", name);
  }
  if (destroy) destroy(instance, nullptr);
  return ok;
}

// SetVulkanLoaderOverride callback: the custom driver's libvulkan, or nullptr (the phone's driver).
inline void* OpenDriver() {
  const std::string name = REXCVAR_GET(wwe2k14_android_driver);
  const std::string lib_dir = REXCVAR_GET(wwe2k14_android_lib_dir);
  if (name.empty()) return nullptr;
  if (lib_dir.empty()) {
    REXLOG_WARN("[driver] {} chosen but no library folder given: the phone's driver is used", name);
    return nullptr;
  }
  const std::string tmp = REXCVAR_GET(wwe2k14_android_tmp_dir);
  // Part 8: an imported driver sits in its own folder; libadrenotools' hooks always come from the library folder.
  std::string driver_dir = REXCVAR_GET(wwe2k14_android_driver_dir);
  if (driver_dir.empty()) driver_dir = lib_dir;
  else if (driver_dir.back() != '/') driver_dir += '/';  // libadrenotools joins folder + name as they are
  REXLOG_INFO("[driver] opening {} from {}", name, driver_dir);
  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp.empty() ? nullptr : tmp.c_str(),
                                            lib_dir.c_str(), driver_dir.c_str(), name.c_str(), nullptr, nullptr);
  if (!handle) {
    REXLOG_WARN("[driver] libadrenotools could not open {} ({}): the phone's driver is used", name, dlerror() ? dlerror() : "");
    return nullptr;
  }
  if (!TestStart(handle, name)) {
    REXLOG_WARN("[driver] {} did not start: the phone's driver is used", name);
    return nullptr;  // the handle stays open: unloading a half-started driver is not safe
  }
  REXLOG_INFO("[driver] using {} (custom driver)", name);
  g_driver_used = name;
  return handle;
}

// OnPreSetup, before anything starts Vulkan.
inline void Install() { rex::ui::vulkan::SetVulkanLoaderOverride(&OpenDriver); }

// Once per guest frame (frame_stats.h): the trial file goes once the game has drawn kTrialFrames frames.
inline void OnFrame() {
  if (g_frames > kTrialFrames) return;
  if (++g_frames == kTrialFrames) {
    const std::string trial = REXCVAR_GET(wwe2k14_android_driver_trial);
    if (!trial.empty() && unlink(trial.c_str()) == 0)
      REXLOG_INFO("[driver] {} frames drawn with {}: trial file removed", kTrialFrames,
                  g_driver_used.empty() ? std::string("the phone's driver") : g_driver_used);
  }
}

}  // namespace wwe2k14::android_driver

#endif  // __ANDROID__
