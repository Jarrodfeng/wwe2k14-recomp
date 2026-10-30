#pragma once
// Session 73 (Jarrod's ideas 2 and 10 of the second ten): what the phone launcher can tell a player before and after a
// game run, in plain words.
// - CheckPhone(): the things that stop the game before its first picture, tested from the launcher - the game files,
//   free storage, memory, the game's 4.6 GB shared memory and 8.6 GB of address space (the SDK's Memory::Initialize,
//   tried the same way), and Vulkan (the phone's own driver: the GPU, its versions, the features the game needs).
// - LastRun(): how the last game run ended - its last start-up step and any crash (logs/startup.txt, src\blackbox.h)
//   and Android's own reason (logs/last_game_exit.txt, written by LauncherActivity from ApplicationExitInfo).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mlauncher::check {

namespace fs = std::filesystem;

struct Line {
  int level;  // 0 fine, 1 a warning, 2 a problem that stops the game
  std::string text;
};

inline std::string Gb(double bytes) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
  return b;
}

// The game's guest memory as the SDK makes it (system/xmemory.cpp Memory::Initialize): one shared memory object of
// 0x120000000 bytes (+ a granule) mapped twice - 4 GB of virtual and 4.5 GB of physical views. Empty = it works.
inline std::string TryGuestMemory() {
#if defined(__ANDROID__)
  using CreateFn = int (*)(const char*, size_t);
  void* android = dlopen("libandroid.so", RTLD_NOW);
  auto create = android ? reinterpret_cast<CreateFn>(dlsym(android, "ASharedMemory_create")) : nullptr;
  if (!create) return "Android has no ASharedMemory_create";
  const size_t size = 0x120000000ull + 0x10000;
  const int fd = create("wwe2k14_check", size);
  if (fd < 0) return "Android refused a 4.6 GB shared memory object";
  std::string error;
  const size_t total = 0x100000000ull + 0x120000000ull;
  void* reserved = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (reserved == MAP_FAILED) {
    error = "Android refused 8.5 GB of address space";
  } else {
    auto* base = static_cast<uint8_t*>(reserved);
    void* a = mmap(base, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    void* b = mmap(base + 0x100000000ull, 0x120000000ull, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (a == MAP_FAILED || b == MAP_FAILED) error = "Android refused to map the game's memory";
    munmap(reserved, total);
  }
  close(fd);
  return error;
#else
  return std::string();
#endif
}

// The kernel's memory page size. Session 73: 16 KB pages (Android 15+, a developer option on Pixel 8 and later, the
// default on some newer phones) are not supported yet. The game's addresses from 0xE0000000 up sit 4 KB further on in
// host memory when pages are bigger than 4 KB (SDK xmemory.cpp, MapViews / PhysicalHeap); the SDK applies that offset
// at run time, but the recompiled code adds it only on Windows and Apple Silicon (rex/system/xmemory.h
// PhysicalHostOffset, the codegen's REX_PHYS_HOST_OFFSET). The game would read and write 4 KB off there.
inline size_t PageSize() {
#if defined(_WIN32)
  return 4096;
#else
  return size_t(sysconf(_SC_PAGESIZE));
#endif
}

inline std::string PageSizeProblem() {
  const size_t page = PageSize();
  if (page <= 4096) return std::string();
  return "This phone uses " + std::to_string(page / 1024) +
         " KB memory pages. The game does not support them yet (it needs 4 KB pages).";
}

inline void CheckMemory(std::vector<Line>& out) {
#if defined(_WIN32)
  MEMORYSTATUSEX m = {sizeof(m)};
  if (!GlobalMemoryStatusEx(&m)) return;
  const double total = double(m.ullTotalPhys), avail = double(m.ullAvailPhys);
#else
  double total = 0, avail = 0;
  std::ifstream f("/proc/meminfo");
  std::string key;
  double kb = 0;
  std::string unit;
  while (f >> key >> kb >> unit) {
    if (key == "MemTotal:") total = kb * 1024.0;
    if (key == "MemAvailable:") avail = kb * 1024.0;
  }
  if (total <= 0) return;
#endif
  const std::string text = "Memory: " + Gb(total) + ", " + Gb(avail) + " free now";
  if (total < 5.5 * 1073741824.0) {
    out.push_back({1, text + ". Under 6 GB: Android may close the game; close other apps first"});
  } else if (avail < 1.5 * 1073741824.0) {
    out.push_back({1, text + ". Little free right now: close other apps before playing"});
  } else {
    out.push_back({0, text});
  }
}

inline std::string DriverVersion(uint32_t vendor, uint32_t v) {
  char b[48];
  if (vendor == 0x10DE) {  // NVIDIA
    std::snprintf(b, sizeof(b), "%u.%u", v >> 22, (v >> 14) & 0xFF);
  } else {  // Qualcomm (512.800.55), ARM, and the Vulkan convention
    std::snprintf(b, sizeof(b), "%u.%u.%u", VK_API_VERSION_MAJOR(v) | (VK_API_VERSION_VARIANT(v) << 7),
                  VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
  }
  return b;
}

// Vulkan with the phone's own driver (a custom driver is loaded by the game only).
inline void CheckVulkan(std::vector<Line>& out) {
#if defined(_WIN32)
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr")) : nullptr;
#else
  void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
#endif
  if (!gipa) {
    out.push_back({2, "Vulkan: not on this phone (the game needs it)"});
    return;
  }
  auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "wwe2k14 phone check";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (!create || create(&info, nullptr, &instance) != VK_SUCCESS) {
    out.push_back({2, "Vulkan: the driver refused to start"});
    return;
  }
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
  auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  auto feats = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(gipa(instance, "vkGetPhysicalDeviceFeatures"));
  uint32_t count = 1;
  VkPhysicalDevice device = VK_NULL_HANDLE;
  if (!enumerate || !props || !feats || (enumerate(instance, &count, &device), count == 0) || !device) {
    out.push_back({2, "Vulkan: no GPU found"});
    if (destroy) destroy(instance, nullptr);
    return;
  }
  VkPhysicalDeviceProperties p = {};
  VkPhysicalDeviceFeatures f = {};
  props(device, &p);
  feats(device, &f);
  char api[24];
  std::snprintf(api, sizeof(api), "%u.%u", VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
  out.push_back({0, std::string("GPU: ") + p.deviceName + ", Vulkan " + api + ", driver " +
                        DriverVersion(p.vendorID, p.driverVersion)});
  // What the SDK refuses a GPU without (ui/vulkan/vulkan_device.cpp; vertex stores and line fill not on phones).
  std::string missing;
  if (!f.independentBlend) missing += " independentBlend";
  if (!f.fragmentStoresAndAtomics) missing += " fragmentStoresAndAtomics";
  if (!f.geometryShader) missing += " geometryShader";
  if (VK_API_VERSION_MINOR(p.apiVersion) < 1 && VK_API_VERSION_MAJOR(p.apiVersion) == 1) missing += " Vulkan 1.1";
  if (!missing.empty()) {
    out.push_back({2, "GPU features the game needs are missing:" + missing});
  } else {
    out.push_back({0, "GPU features: everything the game needs"});
  }
  if (destroy) destroy(instance, nullptr);
}

inline std::vector<Line> CheckPhone(const fs::path& files, bool game_ok, const std::string& game_error,
                                    const std::string& game_name) {
  std::vector<Line> out;
  if (game_ok) {
    out.push_back({0, "Game files: " + (game_name.empty() ? std::string("found") : game_name)});
  } else {
    out.push_back({2, "Game files: " + (game_error.empty() ? std::string("none chosen yet") : game_error)});
  }
  std::error_code ec;
  const auto space = fs::space(files, ec);
  if (!ec) {
    const double free_bytes = double(space.available);
    const std::string text = "Free storage: " + Gb(free_bytes);
    if (free_bytes < 1.0 * 1073741824.0) {
      out.push_back({2, text + ". The game needs about 1 GB for its saves and shader cache"});
    } else if (free_bytes < 3.0 * 1073741824.0) {
      out.push_back({1, text + ". Low: the shader cache grows to about 1 GB"});
    } else {
      out.push_back({0, text});
    }
  }
  CheckMemory(out);
  if (const std::string pages = PageSizeProblem(); !pages.empty()) {
    out.push_back({2, pages});
  } else {
    out.push_back({0, "Memory pages: 4 KB"});
  }
#if defined(__ANDROID__)
  const std::string memory = TryGuestMemory();
  out.push_back(memory.empty() ? Line{0, "The game's 4.6 GB memory block: Android allows it"}
                               : Line{2, "The game's memory block: " + memory + " - the game cannot start here"});
#endif
  CheckVulkan(out);
  return out;
}

// The last game run, in plain words; empty when it ended normally (or there is none).
inline std::string LastRun(const fs::path& files) {
  std::ifstream box(files / "logs" / "startup.txt");
  if (!box) return std::string();
  std::string line, last_step, crash, low_memory;
  bool quit = false, running = false;
  while (std::getline(box, line)) {
    if (const size_t at = line.find("step: "); at != std::string::npos) last_step = line.substr(at + 6);
    if (line.find("CRASH: ") != std::string::npos) crash = line.substr(line.find("CRASH: ") + 7);
    if (line.find("first frame") != std::string::npos) running = true;
    if (line.find("quit to the launcher") != std::string::npos) quit = true;
    if (line.find("memory low") != std::string::npos) low_memory = line;
  }
  std::string reason;
  {
    std::ifstream exit(files / "logs" / "last_game_exit.txt");
    while (std::getline(exit, line)) {
      if (line.rfind("reason: ", 0) == 0) reason = line.substr(8);
    }
  }
  if (quit) return std::string();
  if (!crash.empty()) {
    size_t cut = crash.find(". Backtrace");
    if (cut == std::string::npos && !crash.empty() && crash.back() == '.') cut = crash.size() - 1;
    return "The last game run crashed" + std::string(running ? " while playing" : " at \"" + last_step + "\"") + ": " +
           crash.substr(0, cut) + ".";
  }
  if (reason.rfind("LOW_MEMORY", 0) == 0) {
    return "Android closed the last game run: the phone ran low on memory. Close other apps, or choose a lower "
           "resolution.";
  }
  if (reason.rfind("ANR", 0) == 0) return "The last game run stopped responding (at \"" + last_step + "\").";
  if (reason.rfind("CRASH", 0) == 0) return "The last game run crashed (" + reason + ") at \"" + last_step + "\".";
  if (!running && !last_step.empty()) {
    return "The last game run did not get past \"" + last_step + "\"" + (reason.empty() ? "" : " (" + reason + ")") +
           (low_memory.empty() ? "." : ", with Android warning about low memory.");
  }
  return std::string();
}

}  // namespace mlauncher::check
