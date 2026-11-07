#pragma once
// Session 73 (Jarrod's ideas 2 and 10 of the second ten): what the phone launcher can tell a player before and after a
// game run, in plain words.
// - CheckPhone(): the things that stop the game before its first picture, tested from the launcher - the game files,
//   free storage, memory, the game's 4.6 GB shared memory and 8.6 GB of address space (the SDK's Memory::Initialize,
//   tried the same way), and Vulkan (the phone's own driver: the GPU, its versions, the features the game needs).
// - LastRun(): how the last game run ended - its last start-up step and any crash (logs/startup.txt, src\blackbox.h)
//   and Android's own reason (logs/last_game_exit.txt, written by LauncherActivity from ApplicationExitInfo).
#include <cstdint>
#include <cstdlib>
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
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#include <sys/mman.h>
#include <unistd.h>
#if defined(__ANDROID__) && defined(__aarch64__)
#include <sys/auxv.h>
#endif
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
// at run time, but the recompiled code adds it only on Apple Silicon (rex/system/xmemory.h PhysicalHostOffset, the
// codegen's REX_PHYS_HOST_OFFSET). The game would read and write 4 KB off there.
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
inline void CheckVulkan(std::vector<Line>& out, const std::string& /*custom_driver*/ = std::string()) {
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
  app.apiVersion = VK_API_VERSION_1_2;
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
  // (geometryShader is not required since Session 95 Lane D: vulkan_require_geometry_shader=false on Android, the primitive
  // processor expands rectangle / point lists in the vertex shader without it.)
  if (VK_API_VERSION_MINOR(p.apiVersion) < 1 && VK_API_VERSION_MAJOR(p.apiVersion) == 1) missing += " Vulkan 1.1";
  if (!missing.empty()) {
    out.push_back({2, "GPU features the game needs are missing:" + missing});
  } else {
    out.push_back({0, "GPU features: everything the game needs"});
  }
  // Session 94 (idea 13): the formats TRUE NATIVE's renderer needs. A SMALL COPY of the NONE-fallback core of the SDK's
  // required table (thirdparty\rexglue-sdk-src\src\ui\vulkan\gpu_quirks_apply.cpp, kRequiredRows - that file is the
  // authority; keep this in step). Names a missing format in phone.txt before the player launches.
  auto format_props = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      gipa(instance, "vkGetPhysicalDeviceFormatProperties"));
  if (format_props) {
    constexpr VkFormatFeatureFlags kSampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    constexpr VkFormatFeatureFlags kLinear = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    constexpr VkFormatFeatureFlags kColor = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    constexpr VkFormatFeatureFlags kBlend = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    constexpr VkFormatFeatureFlags kDepth = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    constexpr VkFormatFeatureFlags kXfer = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    struct Need {
      VkFormat format;
      const char* name;
      VkFormatFeatureFlags bits;
      const char* what;
    };
    const Need needs[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8", kColor | kSampled | kLinear | kXfer, "colour target"},
        {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8 blend", kBlend, "blended 8-bit draws"},
        {VK_FORMAT_R16G16B16A16_SNORM, "RGBA16 signed", kColor | kSampled | kXfer, "16-bit colour target"},
        {VK_FORMAT_R8G8B8A8_SNORM, "RGBA8 signed", kSampled, "signed texture views"},
        {VK_FORMAT_R16G16B16A16_SNORM, "RGBA16 signed sampling", kSampled, "signed 16-bit texture views"},
        {VK_FORMAT_R16G16_SNORM, "RG16 signed", kSampled, "signed 16-bit pair views"},
        {VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32 float", kColor | kSampled, "post targets"},
    };
    std::string lacking;
    for (const Need& n : needs) {
      VkFormatProperties fp = {};
      format_props(device, n.format, &fp);
      if ((fp.optimalTilingFeatures & n.bits) != n.bits) lacking += std::string(lacking.empty() ? "" : ", ") + n.name + " (" + n.what + ")";
    }
    // Depth: D32S8 or D24S8, either one, with depth attachment + sampling (EnsurePostObjects' rule); transfers on the chosen one.
    bool depth_ok = false;
    for (VkFormat d : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
      VkFormatProperties fp = {};
      format_props(device, d, &fp);
      const VkFormatFeatureFlags want = kDepth | kSampled;
      if ((fp.optimalTilingFeatures & want) == want) {
        // (the first format that passes is the one the game picks; its transfers are needed separately)
        if (VK_API_VERSION_MINOR(p.apiVersion) >= 1 && (fp.optimalTilingFeatures & kXfer) != kXfer) {
          lacking += std::string(lacking.empty() ? "" : ", ") + "depth transfers";
        }
        depth_ok = true;
        break;
      }
    }
    if (!depth_ok) lacking += std::string(lacking.empty() ? "" : ", ") + "depth + stencil (D32S8 or D24S8)";
    if (!lacking.empty()) {
      out.push_back({1, "GPU formats missing (system driver; a custom driver may differ; the game may show a black screen or lose effects): " + lacking});
    } else {
      out.push_back({0, "GPU formats: the colour, depth and post formats the game draws with are there (system driver; a custom driver may differ)"});
    }
  }
  if (destroy) destroy(instance, nullptr);
}

// Session 95 (LADDER, plan item 6): the game library is built with -march=armv8.2-a (tools\android\build_game_android.ps1,
// build_sdk_android.ps1), so a CPU without LSE atomics and half-precision float dies on its first such instruction with SIGILL and
// no explanation. The launcher itself is built for plain armv8-a, so it survives to say so. Empty = fine (or not an arm64 phone).
inline std::string CpuProblem() {
#if defined(__ANDROID__) && defined(__aarch64__)
  const unsigned long hw = getauxval(AT_HWCAP);
  std::string missing;
  if (!(hw & (1ul << 8))) missing += " atomics";            // HWCAP_ATOMICS (armv8.1 LSE)
  if (!(hw & (1ul << 9))) missing += " half-precision-float";  // HWCAP_FPHP
  if (!(hw & (1ul << 10))) missing += " half-precision-vectors";  // HWCAP_ASIMDHP
  if (!missing.empty())
    return "This phone's processor is too old for the game (it lacks:" + missing +
           "). The game needs an Armv8.2 processor (phones from about 2018 on) and cannot run here.";
#endif
  return std::string();
}

inline std::vector<Line> CheckPhone(const fs::path& files, bool game_ok, const std::string& game_error,
                                    const std::string& game_name, const std::string& custom_driver = std::string()) {
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
  if (const std::string cpu = CpuProblem(); !cpu.empty()) out.push_back({2, cpu});
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
  CheckVulkan(out, custom_driver);
  return out;
}

// Session 95 (DEATH): a signal number from Android's exit status, in words; empty for a number that is not a crash
// (9 = SIGKILL is the low-memory killer's and stays out; so does 0).
inline std::string CrashSignalWords(int status) {
  switch (status) {
    case 4: return "SIGILL, an illegal instruction";
    case 5: return "SIGTRAP, a trap";
    case 6: return "SIGABRT, the game stopped itself";
    case 7: return "SIGBUS, a bad memory access";
    case 8: return "SIGFPE, an arithmetic error";
    case 11: return "SIGSEGV, a bad memory access";
    case 31: return "SIGSYS, a blocked system call";
    default: return std::string();
  }
}

// The Android API level; -1 when not asked for (the PC build, tests).
inline int AndroidSdkLevel() {
#if defined(__ANDROID__)
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get("ro.build.version.sdk", value) > 0) return std::atoi(value);
#endif
  return -1;
}

// The last game run, in plain words; empty when it ended normally (or there is none).
// sdk_level: the Android API level (Session 95: below 30 there is no exit reason, so the file says nothing); -1 asks the phone.
inline std::string LastRun(const fs::path& files, int sdk_level = -1) {
  std::ifstream box(files / "logs" / "startup.txt");
  if (!box) return std::string();
  if (sdk_level < 0) sdk_level = AndroidSdkLevel();
  // Session 95 (DEATH): logs/stderr.txt (src\blackbox.h) holds assert.h's "FATAL ERROR: <message>" line, written just
  // before the game aborted. It names deaths that have no CRASH line (the SDK's own fatal errors).
  std::string fatal;
  {
    std::ifstream err(files / "logs" / "stderr.txt");
    std::string e;
    while (std::getline(err, e)) {
      if (const size_t at = e.find("FATAL ERROR: "); at != std::string::npos) fatal = e.substr(at + 13);
    }
    while (!fatal.empty() && (fatal.back() == '\r' || fatal.back() == ' ')) fatal.pop_back();
    if (fatal.size() > 300) fatal.resize(300);
  }
  std::string line, last_step, crash, low_memory, gpu_fatal, hang, black, dark;
  bool quit = false, running = false;
  while (std::getline(box, line)) {
    if (const size_t at = line.find("step: "); at != std::string::npos) last_step = line.substr(at + 6);
    if (line.find("CRASH: ") != std::string::npos) crash = line.substr(line.find("CRASH: ") + 7);
    if (line.find("first frame") != std::string::npos) running = true;
    if (line.find("quit to the launcher") != std::string::npos) quit = true;
    if (line.find("memory low") != std::string::npos) low_memory = line;
    if (const size_t at = line.find("FATAL: GPU lacks "); at != std::string::npos) gpu_fatal = line.substr(at + 17);  // Session 95 (PLAIN)
    // Session 95 (HANG, src\hang_guard.h): a freeze or a black picture that was still there when the run ended.
    if (const size_t at = line.find("HANG: "); at != std::string::npos) hang = line.substr(at + 6);
    if (line.find("HANG over") != std::string::npos) hang.clear();
    if (const size_t at = line.find("BLACK: dark picture"); at != std::string::npos) {
      dark = line.substr(at + 7);  // (F2) a guess from 3 captures: counts only when the run did not end in a quit
    } else if (const size_t at2 = line.find("BLACK: "); at2 != std::string::npos) {
      black = line.substr(at2 + 7);
    }
    if (line.find("BLACK over") != std::string::npos) black.clear();
  }
  const std::string froze = !hang.empty() ? "The last game run froze: " + hang + "."
                            : !black.empty() ? "The picture went black in the last game run: " + black + "."
                                             : std::string();
  std::string reason;
  int exit_status = 0;
  {
    // Session 95 (DEATH): only an exit record written after this run started belongs to it (an older run's record
    // must not name a run that is still alive).
    std::error_code ec1, ec2;
    const auto exit_time = fs::last_write_time(files / "logs" / "last_game_exit.txt", ec1);
    const auto start_time = fs::last_write_time(files / "logs" / "startup.txt", ec2);
    std::ifstream exit;
    if (!ec1 && (ec2 || exit_time >= start_time)) exit.open(files / "logs" / "last_game_exit.txt");
    while (std::getline(exit, line)) {
      if (line.rfind("reason: ", 0) == 0) reason = line.substr(8);
      if (line.rfind("status: ", 0) == 0) exit_status = std::atoi(line.c_str() + 8);
    }
  }
  if (!gpu_fatal.empty()) return "The last game run ended because this phone's GPU lacks " + gpu_fatal + ".";
  if (quit) return froze;  // (a blind quit from a frozen or black game is still a frozen or black game)
  if (!fatal.empty()) {
    return "The last game run stopped on a fatal error" +
           std::string(running ? " while playing" : " at \"" + last_step + "\"") + ": " + fatal;
  }
  if (!crash.empty()) {
    size_t cut = crash.find(". Backtrace");
    if (cut == std::string::npos && !crash.empty() && crash.back() == '.') cut = crash.size() - 1;
    return "The last game run crashed" + std::string(running ? " while playing" : " at \"" + last_step + "\"") + ": " +
           crash.substr(0, cut) + ".";
  }
  // Session 95 (DEATH): a signal that is a crash (not SIGKILL) is a crash, never "low memory" (report_005: SIGNALED 11
  // with memory warnings in the file was read as low memory).
  if (reason.rfind("SIGNALED", 0) == 0 || reason.rfind("CRASH", 0) == 0) {
    const std::string words = CrashSignalWords(exit_status);
    if (!words.empty()) {
      return "The last game run crashed" + std::string(running ? " while playing" : " at \"" + last_step + "\"") +
             " (" + words + ", no crash line was written).";
    }
  }
  if (!froze.empty()) return froze;
  if (!dark.empty()) return "The picture may have gone black in the last game run: " + dark + ".";
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
  // Session 95 (DEATH): API 28-29 give no exit reason, so a run that played and left no quit marker would show nothing.
  // A message only: it never counts as a failure on the ladder (LADDER reads the crash / fatal / hang lines, not this).
  if (running && reason.empty() && sdk_level >= 28 && sdk_level <= 29) {
    return "The last game run ended unexpectedly (this Android version does not say why).";
  }
  return std::string();
}

}  // namespace mlauncher::check
