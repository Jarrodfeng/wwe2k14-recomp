// Settings parity S10b: NVIDIA DLSS / DLAA through Streamline in the new renderer (streamline.h). The calls follow the SDK's Session 18 / 24
// module (thirdparty\rexglue-sdk-src\src\ui\vulkan\streamline.cpp): secure load, manual hooking, frame-based tags, DLSS options per frame.
#include "streamline.h"

#include "problem.h"

#if defined(RDR_HAS_STREAMLINE) && defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

#include <sl.h>
#include <sl_dlss.h>
#include <sl_security.h>

#include <cstring>
#include <filesystem>
#include <mutex>

namespace rdr::streamline {
namespace {

HMODULE g_module = nullptr;
bool g_active = false;
int g_dlss = -1;  // -1 unknown, 0 no, 1 yes
std::mutex g_mutex;
PFun_slInit* p_slInit = nullptr;
PFun_slShutdown* p_slShutdown = nullptr;
PFun_slIsFeatureSupported* p_slIsFeatureSupported = nullptr;
PFun_slGetFeatureFunction* p_slGetFeatureFunction = nullptr;
PFun_slGetNewFrameToken* p_slGetNewFrameToken = nullptr;
PFun_slSetConstants* p_slSetConstants = nullptr;
PFun_slSetTagForFrame* p_slSetTagForFrame = nullptr;
PFun_slEvaluateFeature* p_slEvaluateFeature = nullptr;
PFun_slDLSSSetOptions* p_slDLSSSetOptions = nullptr;
std::wstring g_dir;

void LogCallback(sl::LogType type, const char* message) {
  std::string m(message ? message : "");
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  if (type == sl::LogType::eError || type == sl::LogType::eWarn) Log("[rdr][dlss] streamline: %s", m.c_str());
}

template <typename T>
bool Get(T*& out, const char* name) {
  out = reinterpret_cast<T*>(reinterpret_cast<void*>(GetProcAddress(g_module, name)));
  if (!out) Log("[rdr][dlss] %s not found in sl.interposer.dll", name);
  return out != nullptr;
}

sl::Resource ToResource(const Image& i, uint32_t w, uint32_t h) {
  sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(i.image), reinterpret_cast<void*>(i.memory), reinterpret_cast<void*>(i.view),
                 uint32_t(i.layout));
  r.width = w, r.height = h, r.nativeFormat = uint32_t(i.format), r.mipLevels = 1, r.arrayLayers = 1;
  r.flags = i.flags, r.usage = i.usage;
  return r;
}

}  // namespace

PFN_vkGetInstanceProcAddr Init(const std::string& dir_utf8) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (dir_utf8.empty() || g_module) return nullptr;
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::absolute(std::filesystem::u8path(dir_utf8), ec);
  const std::filesystem::path interposer = dir / "sl.interposer.dll";
  if (!std::filesystem::is_regular_file(interposer, ec)) {
    Log("[rdr][dlss] %s not found: DLSS not available", interposer.string().c_str());
    return nullptr;
  }
  if (!sl::security::verifyEmbeddedSignature(interposer.wstring().c_str())) {  // a module without NVIDIA's signature is never loaded
    Log("[rdr][dlss] %s is not signed by NVIDIA: not loaded", interposer.string().c_str());
    return nullptr;
  }
  g_module = LoadLibraryW(interposer.wstring().c_str());
  if (!g_module) {
    Log("[rdr][dlss] LoadLibrary(%s) failed (%lu)", interposer.string().c_str(), GetLastError());
    return nullptr;
  }
  const bool ok = Get(p_slInit, "slInit") && Get(p_slShutdown, "slShutdown") && Get(p_slIsFeatureSupported, "slIsFeatureSupported") &&
                  Get(p_slGetFeatureFunction, "slGetFeatureFunction") && Get(p_slGetNewFrameToken, "slGetNewFrameToken") &&
                  Get(p_slSetConstants, "slSetConstants") && Get(p_slSetTagForFrame, "slSetTagForFrame") &&
                  Get(p_slEvaluateFeature, "slEvaluateFeature");
  auto proxy = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(g_module, "vkGetInstanceProcAddr")));
  if (!ok || !proxy) {
    FreeLibrary(g_module);
    g_module = nullptr;
    return nullptr;
  }
  g_dir = dir.wstring();
  static const wchar_t* paths[1];
  paths[0] = g_dir.c_str();
  static const sl::Feature features[] = {sl::kFeatureDLSS};
  sl::Preferences pref{};
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.pathsToPlugins = paths;
  pref.numPathsToPlugins = 1;
  pref.logMessageCallback = &LogCallback;
  // Manual hooking (the renderer makes its own instance and device through the proxy). No over-the-air updates: nothing is downloaded.
  pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = 1;
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "wwe2k14-rdr";
  pref.projectId = "4e1c3b7a-2f5d-4c86-9a1e-7b3d0f6c2a91";  // the same local id as the SDK's integration
  pref.renderAPI = sl::RenderAPI::eVulkan;
  const sl::Result r = p_slInit(pref, sl::kSDKVersion);
  if (r != sl::Result::eOk) {
    Log("[rdr][dlss] slInit failed (%d): DLSS not available", int(r));
    FreeLibrary(g_module);
    g_module = nullptr;
    return nullptr;
  }
  g_active = true;
  Log("[rdr][dlss] Streamline loaded from %s (manual hooking): the renderer's Vulkan goes through it", dir.string().c_str());
  return proxy;
}

bool Active() { return g_active; }

bool Available(VkPhysicalDevice physical_device) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_active) return false;
  if (g_dlss >= 0) return g_dlss == 1;
  sl::AdapterInfo adapter{};
  adapter.vkPhysicalDevice = reinterpret_cast<void*>(physical_device);
  const sl::Result r = p_slIsFeatureSupported(sl::kFeatureDLSS, adapter);
  void* fn = nullptr;
  const sl::Result rf = r == sl::Result::eOk ? p_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn) : r;
  p_slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
  g_dlss = (r == sl::Result::eOk && rf == sl::Result::eOk && p_slDLSSSetOptions) ? 1 : 0;
  Log("[rdr][dlss] DLSS %s on this device (slIsFeatureSupported %d, slDLSSSetOptions %d)", g_dlss ? "available" : "NOT available", int(r), int(rf));
  return g_dlss == 1;
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_active && p_slShutdown) p_slShutdown();
  g_active = false;
}

bool Evaluate(VkCommandBuffer command_buffer, const Frame& f, std::string* reason) {
  if (!g_active || g_dlss != 1) {
    if (reason) *reason = "DLSS not available";
    return false;
  }
  sl::FrameToken* token = nullptr;
  const uint32_t index = f.frame_index;
  if (p_slGetNewFrameToken(token, &index) != sl::Result::eOk || !token) {
    if (reason) *reason = "slGetNewFrameToken failed";
    return false;
  }
  const sl::ViewportHandle viewport(0);
  static const sl::DLSSMode kModes[] = {sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eBalanced,
                                        sl::DLSSMode::eMaxQuality, sl::DLSSMode::eUltraPerformance, sl::DLSSMode::eUltraQuality};
  sl::DLSSOptions options{};
  options.mode = kModes[(f.mode >= 0 && f.mode < 6) ? f.mode : 0];
  options.outputWidth = f.out_width, options.outputHeight = f.out_height;
  options.colorBuffersHDR = sl::Boolean::eFalse;  // the game's picture: 8 bits a channel, after its own post effects
  sl::Result r = p_slDLSSSetOptions(viewport, options);
  if (r != sl::Result::eOk) {
    if (reason) *reason = "slDLSSSetOptions failed (" + std::to_string(int(r)) + ")";
    return false;
  }
  sl::Constants c{};
  std::memcpy(&c.cameraViewToClip, f.view_to_clip, 64);
  std::memcpy(&c.clipToCameraView, f.clip_to_view, 64);
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::memcpy(&c.clipToLensClip, identity, 64);
  std::memcpy(&c.clipToPrevClip, f.clip_to_prev_clip, 64);
  std::memcpy(&c.prevClipToClip, f.prev_clip_to_clip, 64);
  c.jitterOffset = {f.jitter[0], f.jitter[1]};
  c.mvecScale = {f.mvec_scale[0], f.mvec_scale[1]};
  c.cameraPinholeOffset = {0, 0};
  c.cameraPos = {0, 0, 0};
  c.cameraUp = {0, 1, 0};
  c.cameraRight = {1, 0, 0};
  c.cameraFwd = {0, 0, 1};
  c.cameraNear = 1.0f, c.cameraFar = 10000.0f;
  c.cameraFOV = 1.0f, c.cameraAspectRatio = f.height ? float(f.width) / float(f.height) : 1.0f;
  c.depthInverted = sl::Boolean::eTrue;  // the game's depth is reversed (near = 1): the old path's setting (command_processor.cpp 5297)
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  r = p_slSetConstants(c, *token, viewport);
  if (r != sl::Result::eOk) {
    if (reason) *reason = "slSetConstants failed (" + std::to_string(int(r)) + ")";
    return false;
  }
  sl::Resource color_in = ToResource(f.color_in, f.width, f.height);
  sl::Resource depth = ToResource(f.depth, f.width, f.height);
  sl::Resource motion = ToResource(f.motion, f.width, f.height);
  sl::Resource color_out = ToResource(f.color_out, f.out_width, f.out_height);
  const sl::Extent extent{0, 0, f.width, f.height};
  const sl::Extent out_extent{0, 0, f.out_width, f.out_height};
  sl::ResourceTag tags[4] = {
      sl::ResourceTag(&color_in, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &extent),
      sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent),
      sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent),
      sl::ResourceTag(&color_out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &out_extent)};
  auto* cb = reinterpret_cast<sl::CommandBuffer*>(command_buffer);
  r = p_slSetTagForFrame(*token, viewport, tags, 4, cb);
  if (r != sl::Result::eOk) {
    if (reason) *reason = "slSetTagForFrame failed (" + std::to_string(int(r)) + ")";
    return false;
  }
  const sl::BaseStructure* inputs[] = {&viewport};
  r = p_slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, cb);
  if (r != sl::Result::eOk) {
    if (reason) *reason = "slEvaluateFeature failed (" + std::to_string(int(r)) + ")";
    return false;
  }
  return true;
}

}  // namespace rdr::streamline

#else  // no Streamline in this build (Android, rplay): DLSS is never available

namespace rdr::streamline {
PFN_vkGetInstanceProcAddr Init(const std::string&) { return nullptr; }
bool Active() { return false; }
bool Available(VkPhysicalDevice) { return false; }
void Shutdown() {}
bool Evaluate(VkCommandBuffer, const Frame&, std::string* reason) {
  if (reason) *reason = "this build has no Streamline";
  return false;
}
}  // namespace rdr::streamline

#endif
