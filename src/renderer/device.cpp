#include "device.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <string>

#include "dyn_state.h"  // S111 DYNSTATE
#include "format_caps.h"
#include "log_formats.h"  // S101 lane 19 review: shared with the case zip
#include "problem.h"
#include "streamline.h"

namespace rdr {
namespace {

std::atomic<uint64_t> g_validation_errors{0};

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    g_validation_errors.fetch_add(1, std::memory_order_relaxed);
    Log("[rdr][validation] ERROR %s", data->pMessage);
    for (uint32_t i = 0; i < data->objectCount; ++i) {  // named objects (pipelines carry their program's name): which shader it was
      if (data->pObjects[i].pObjectName) Log("[rdr][validation]   object %u: %s", unsigned(data->pObjects[i].objectType), data->pObjects[i].pObjectName);
    }
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    Log("[rdr][validation] warning %s", data->pMessage);
  }
  return VK_FALSE;
}

// S102 U1: `part` (the launcher's graphics card choice) found in a device name, any case.
bool NameHas(const char* name, const std::string& part) {
  std::string n(name), p(part);
  for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (char& c : p) c = char(std::tolower(static_cast<unsigned char>(c)));
  return n.find(p) != std::string::npos;
}

bool InstanceHas(const char* name) {  // S102 G2b F3
  uint32_t count = 0;
  vk::vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> exts(count);
  vk::vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
  for (const auto& e : exts) {
    if (std::strcmp(e.extensionName, name) == 0) return true;
  }
  return false;
}

bool HasLayer(const char* name) {
  uint32_t count = 0;
  vk::vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vk::vkEnumerateInstanceLayerProperties(&count, layers.data());
  for (const auto& layer : layers) {
    if (std::strcmp(layer.layerName, name) == 0) return true;
  }
  return false;
}

// The floor (docs\renderer\FLOOR.md). Every item is either required here or not used anywhere.
void CheckFloor(VkPhysicalDevice gpu, const VkPhysicalDeviceProperties& props, const VkPhysicalDeviceFeatures& features, std::vector<std::string>& missing) {
  if (props.apiVersion < VK_API_VERSION_1_1) missing.push_back("Vulkan 1.1");
  if (props.limits.maxBoundDescriptorSets < 4) missing.push_back("4 descriptor sets");
  if (props.limits.maxImageDimension2D < 4096) missing.push_back("4096 pixel images");
  if (props.limits.maxColorAttachments < 4) missing.push_back("4 colour attachments");
  if (props.limits.maxUniformBufferRange < 16384) missing.push_back("16 KB uniform range");
  VkFormatProperties rgba8{};
  vk::vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R8G8B8A8_UNORM, &rgba8);
  const VkFormatFeatureFlags rgba8_needs = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((rgba8.optimalTilingFeatures & rgba8_needs) != rgba8_needs) missing.push_back("RGBA8 render and sample");
  // The game's 16-bit shadow targets. Our own format choice, because we write both ends: Vulkan's required list covers this one on every device.
  VkFormatProperties rgba16{};
  vk::vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R16G16B16A16_SFLOAT, &rgba16);
  const VkFormatFeatureFlags rgba16_needs = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((rgba16.optimalTilingFeatures & rgba16_needs) != rgba16_needs) missing.push_back("RGBA16F render and sample");
}

VkFormat PickDepthFormat(VkPhysicalDevice gpu) {
  const VkFormatFeatureFlags needs = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  // Float first: the game's depth is reversed and 20e4 float, and 32-bit float draws the same pictures (GAME_RULES_DRAWS D6, D8).
  for (VkFormat format : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
    VkFormatProperties props{};
    vk::vkGetPhysicalDeviceFormatProperties(gpu, format, &props);
    if ((props.optimalTilingFeatures & needs) == needs) return format;
  }
  return VK_FORMAT_UNDEFINED;
}

}  // namespace

uint64_t Device::validation_errors() const { return g_validation_errors.load(std::memory_order_relaxed); }

bool Device::Create(const DeviceOptions& options, std::string& error) {
  format_caps_mask_ = options.format_caps_mask;
  // S10b: with a Streamline folder the instance and device are made through its interposer (it adds what DLSS needs).
  if (!options.streamline_dir.empty()) vk::SetInstanceProcAddrOverride(streamline::Init(options.streamline_dir));
  if (!vk::LoadVulkan() || !vk::vkEnumerateInstanceVersion) {
    error = "This device does not have Vulkan 1.1.";
    return false;
  }
  uint32_t loader_version = 0;
  vk::vkEnumerateInstanceVersion(&loader_version);
  if (loader_version < VK_API_VERSION_1_1) {
    error = "This device does not have Vulkan 1.1.";
    return false;
  }

  // ---- instance ----
  std::vector<const char*> instance_extensions;
  std::vector<const char*> layers;
  bool has_caps2 = false;  // S102 G2b
  if (options.windowed) {
    instance_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
    instance_extensions.push_back("VK_KHR_win32_surface");
    // S102 G2b F3: the exclusive-fullscreen switch needs VK_KHR_get_surface_capabilities2 (to read fullScreenExclusiveSupported). Without it the device extension stays off: `unsupported`.
    if (options.exclusive_fullscreen && InstanceHas("VK_KHR_get_surface_capabilities2")) {
      instance_extensions.push_back("VK_KHR_get_surface_capabilities2");
      has_caps2 = true;
    }
#elif defined(__ANDROID__)
    instance_extensions.push_back("VK_KHR_android_surface");
#endif
  }
  const bool validate = options.validation > 0 && HasLayer("VK_LAYER_KHRONOS_validation");
  if (options.validation > 0 && !validate) Log("[rdr] validation asked for, but the layer is not installed");
  if (validate) {
    layers.push_back("VK_LAYER_KHRONOS_validation");
    instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    instance_extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "WWE 2K14";
  app.pEngineName = "rdr";
  app.apiVersion = VK_API_VERSION_1_1;
  VkValidationFeatureEnableEXT validation_enables[2] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT, VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT};
  VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
  validation_features.enabledValidationFeatureCount = options.validation >= 2 ? 2 : 1;
  validation_features.pEnabledValidationFeatures = validation_enables;
  // S102 D1 (DEVICE_LOST.md, the PC route): level 3 = GPU-assisted validation (out-of-range index / vertex fetches, descriptor and buffer-address checks done by
  // instrumented shaders) instead of synchronisation validation (the layer does not run both at once). Offline tools only (rplay --validation 3).
  VkValidationFeatureEnableEXT gpu_av[2] = {VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT, VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT};
  if (options.validation >= 3) validation_features.enabledValidationFeatureCount = 2, validation_features.pEnabledValidationFeatures = gpu_av;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pNext = validate ? &validation_features : nullptr;
  instance_info.pApplicationInfo = &app;
  instance_info.enabledLayerCount = uint32_t(layers.size());
  instance_info.ppEnabledLayerNames = layers.data();
  instance_info.enabledExtensionCount = uint32_t(instance_extensions.size());
  instance_info.ppEnabledExtensionNames = instance_extensions.data();
  if (vk::vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
    error = "Vulkan could not start on this device.";
    return false;
  }
  vk::LoadInstance(instance_);
  if (validate) {
    VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger.pfnUserCallback = OnValidation;
    vk::vkCreateDebugUtilsMessengerEXT(instance_, &messenger, nullptr, &messenger_);
  }

  // ---- physical device: the best one that clears the floor ----
  uint32_t gpu_count = 0;
  vk::vkEnumeratePhysicalDevices(instance_, &gpu_count, nullptr);
  std::vector<VkPhysicalDevice> gpus(gpu_count);
  vk::vkEnumeratePhysicalDevices(instance_, &gpu_count, gpus.data());
  int best_score = -1;
  bool named_found = false;
  std::vector<std::string> best_missing;
  for (uint32_t i = 0; i < gpu_count; ++i) {
    if (options.gpu_index >= 0 && int(i) != options.gpu_index) continue;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceFeatures features{};
    vk::vkGetPhysicalDeviceProperties(gpus[i], &props);
    vk::vkGetPhysicalDeviceFeatures(gpus[i], &features);
    std::vector<std::string> missing;
    CheckFloor(gpus[i], props, features, missing);
    if (PickDepthFormat(gpus[i]) == VK_FORMAT_UNDEFINED) missing.push_back("a sampled depth format");
    if (!missing.empty()) {
      if (best_missing.empty()) best_missing = missing;
      continue;
    }
    int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
    // S102 U1: the launcher's card choice (R9 finding 3: only the SDK's device honoured it, and the SDK has no device any more). A name match outranks every type score.
    if (!options.gpu_name.empty() && NameHas(props.deviceName, options.gpu_name)) {
      score = 100;
      named_found = true;
    }
    if (score > best_score) {
      best_score = score;
      physical_ = gpus[i];
      properties_ = props;
    }
  }
  if (physical_ == VK_NULL_HANDLE) {
    error = "This graphics hardware is below the minimum.";
    if (!best_missing.empty()) {
      error += " Missing:";
      for (size_t i = 0; i < best_missing.size(); ++i) error += (i ? ", " : " ") + best_missing[i];
      error += ".";
    }
    return false;
  }
  if (!options.gpu_name.empty())
    Log("[rdr] graphics card: asked for '%s' (the launcher): %s%s", options.gpu_name.c_str(), named_found ? "using " : "no card clears the floor with that name, using ", properties_.deviceName);
  VkPhysicalDeviceFeatures available{};
  vk::vkGetPhysicalDeviceFeatures(physical_, &available);
  vk::vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

  // ---- queue ----
  uint32_t family_count = 0;
  vk::vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vk::vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
  bool found_queue = false;
  for (uint32_t i = 0; i < family_count && !found_queue; ++i) {
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_ = i;
      found_queue = true;
      caps_.compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;   // S102 L1b: the frame's queue can dispatch (every graphics family does in practice)
    }
  }
  if (!found_queue) {
    error = "This graphics hardware has no graphics queue.";
    return false;
  }

  // ---- device: the floor, plus compressed texture support when the hardware has it ----
  VkPhysicalDeviceFeatures enabled{};
  enabled.depthClamp = available.depthClamp;
  enabled.occlusionQueryPrecise = options.occlusion_precise && available.occlusionQueryPrecise;  // S102 G11 (F1): only when the offline frame trace asks (DeviceOptions::occlusion_precise)
  enabled.samplerAnisotropy = available.samplerAnisotropy;
  enabled.robustBufferAccess = options.robust_buffers && available.robustBufferAccess;  // S102 D1: only when --wwe2k14_rdr_robust_buffers asks (DEVICE_LOST.md job C)
  if (options.robust_buffers) Log("[rdr] robustBufferAccess %s (--wwe2k14_rdr_robust_buffers)", enabled.robustBufferAccess ? "ON" : "asked for, but the device does not offer it");
  enabled.pipelineStatisticsQuery = options.pipeline_statistics && available.pipelineStatisticsQuery;  // S102 G10 (review F11): only when rplay asks; the game's device is unchanged
  enabled.textureCompressionBC = available.textureCompressionBC;
  enabled.textureCompressionASTC_LDR = available.textureCompressionASTC_LDR;
  // S102 C1 (layered_targets.h; review F2): multiview (core in Vulkan 1.1, no extension to name) is enabled ONLY when the caller asked (DeviceOptions::multiview: wwe2k14_rdr_prelight_layered on at start) AND the
  // device has it; with the switch off the device creation below is exactly what it was (no feature query, no pNext). The pre-light cube bake renders its six faces in one pass through it;
  // maxMultiviewViewCount is at least 6 whenever the feature exists.
  VkPhysicalDeviceMultiviewFeatures multiview_available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
  if (options.multiview && vk::vkGetPhysicalDeviceFeatures2) {
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &multiview_available;
    vk::vkGetPhysicalDeviceFeatures2(physical_, &query);
  }
  VkPhysicalDeviceMultiviewFeatures multiview_enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
  multiview_enabled.multiview = options.multiview && multiview_available.multiview;   // guarded by options.
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  // Phone 120 fix (S100, runs s100ph1 / s100ph2): Android took our plain FIFO presents at 60 a second on a 120 Hz screen; the old presenter
  // reached 120 with desired present times (VK_GOOGLE_display_timing). Enabled when the device has it (swapchain.cpp Present).
  bool has_timing = false, has_exclusive = false;
  if (options.windowed) {
    uint32_t ext_count = 0;
    vk::vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vk::vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count, exts.data());
    for (const auto& e : exts) has_timing |= std::strcmp(e.extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) == 0;
    {  // S102 D1 (DEVICE_LOST.md): what a lost device could be asked, logged once; nothing enabled (the player's device is unchanged)
      bool fault = false, budget = false;
      for (const auto& e : exts) fault |= std::strcmp(e.extensionName, "VK_EXT_device_fault") == 0, budget |= std::strcmp(e.extensionName, "VK_EXT_memory_budget") == 0;
      Log("[rdr] device offers VK_EXT_device_fault: %s, VK_EXT_memory_budget: %s (lost-device diagnosis; neither enabled)", fault ? "yes" : "no", budget ? "yes" : "no");
    }
#if defined(_WIN32)
    // S102 G2b: only when the switch asked for it and the instance has VK_KHR_get_surface_capabilities2 (F3); otherwise the flag stays false and the swapchain logs "unsupported".
    if (options.exclusive_fullscreen && has_caps2)
      for (const auto& e : exts) has_exclusive |= std::strcmp(e.extensionName, "VK_EXT_full_screen_exclusive") == 0;
#endif
  }
  // S111 DYNSTATE (dyn_state.h): which draw states this device can set per draw. Asked once, here: the answer (Caps::dyn_mask) says for the whole session what a
  // pipeline bakes and what the record sets. Not behind options.windowed: the offline tools' headless device builds the same pipelines.
  dyn::Report dyn_report;
  VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds1_has{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
  VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3_has{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
  {
    uint32_t all_count = 0;
    vk::vkEnumerateDeviceExtensionProperties(physical_, nullptr, &all_count, nullptr);
    std::vector<VkExtensionProperties> all(all_count);
    vk::vkEnumerateDeviceExtensionProperties(physical_, nullptr, &all_count, all.data());
    for (uint32_t i = 0; i < all_count && i < all.size(); ++i) {
      dyn_report.ext1 |= std::strcmp(all[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0;
      dyn_report.ext3 |= std::strcmp(all[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) == 0;
    }
    if ((dyn_report.ext1 || dyn_report.ext3) && vk::vkGetPhysicalDeviceFeatures2) {   // a feature struct is chained only for an extension the device lists
      VkPhysicalDeviceFeatures2 dyn_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      if (dyn_report.ext1) eds1_has.pNext = dyn_query.pNext, dyn_query.pNext = &eds1_has;
      if (dyn_report.ext3) eds3_has.pNext = dyn_query.pNext, dyn_query.pNext = &eds3_has;
      vk::vkGetPhysicalDeviceFeatures2(physical_, &dyn_query);
      dyn_report.feature1 = eds1_has.extendedDynamicState != 0;
      dyn_report.blend_enable = eds3_has.extendedDynamicState3ColorBlendEnable != 0;
      dyn_report.blend_equation = eds3_has.extendedDynamicState3ColorBlendEquation != 0;
      dyn_report.write_mask = eds3_has.extendedDynamicState3ColorWriteMask != 0;
    }
  }
  uint32_t dyn_mask = dyn::MaskOf(dyn_report, options.dynamic_state);
  VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds1_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
  eds1_on.extendedDynamicState = VK_TRUE;
  VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
  eds3_on.extendedDynamicState3ColorBlendEnable = VK_TRUE, eds3_on.extendedDynamicState3ColorBlendEquation = VK_TRUE, eds3_on.extendedDynamicState3ColorWriteMask = VK_TRUE;
  const char* device_extensions[5] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, nullptr, nullptr, nullptr, nullptr};
  uint32_t device_extension_count = 0;
  if (options.windowed) {
    device_extension_count = 1;
    if (has_timing) device_extensions[device_extension_count++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
    if (has_exclusive) device_extensions[device_extension_count++] = "VK_EXT_full_screen_exclusive";  // S102 G2b
  }
  if (dyn_mask & dyn::kRaster) device_extensions[device_extension_count++] = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;     // S111 DYNSTATE
  if (dyn_mask & dyn::kBlend) device_extensions[device_extension_count++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
  void* feature_chain = multiview_enabled.multiview ? &multiview_enabled : nullptr;   // S102 C1
  if (dyn_mask & dyn::kRaster) eds1_on.pNext = feature_chain, feature_chain = &eds1_on;   // S111 DYNSTATE: only the groups in use are enabled
  if (dyn_mask & dyn::kBlend) eds3_on.pNext = feature_chain, feature_chain = &eds3_on;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = device_extension_count;
  device_info.ppEnabledExtensionNames = device_extensions;
  device_info.pEnabledFeatures = &enabled;
  device_info.pNext = feature_chain;
  if (vk::vkCreateDevice(physical_, &device_info, nullptr, &device_) != VK_SUCCESS) {
    error = "The graphics driver refused to create a device.";
    return false;
  }
  dlss_ = streamline::Active() && streamline::Available(physical_);  // S10b
  caps_.display_timing = has_timing;
  caps_.full_screen_exclusive = has_exclusive;  // S102 G2b
  vk::LoadDevice(device_);
  vk::vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  // S111 DYNSTATE: a group whose commands did not load is not used
  if ((dyn_mask & dyn::kRaster) && !(vk::vkCmdSetCullModeEXT && vk::vkCmdSetFrontFaceEXT && vk::vkCmdSetDepthTestEnableEXT && vk::vkCmdSetDepthWriteEnableEXT && vk::vkCmdSetDepthCompareOpEXT))
    dyn_mask &= ~uint32_t(dyn::kRaster);
  if ((dyn_mask & dyn::kBlend) && !(vk::vkCmdSetColorBlendEnableEXT && vk::vkCmdSetColorBlendEquationEXT && vk::vkCmdSetColorWriteMaskEXT)) dyn_mask &= ~uint32_t(dyn::kBlend);
  caps_.dyn_mask = dyn_mask;
  Log("[rdr] dynamic state: VK_EXT_extended_dynamic_state listed %d feature %d; VK_EXT_extended_dynamic_state3 listed %d blend enable %d equation %d write mask %d; allowed %X; in use: cull/depth %d, blend %d (a state not in use stays in the pipeline key)",
      int(dyn_report.ext1), int(dyn_report.feature1), int(dyn_report.ext3), int(dyn_report.blend_enable), int(dyn_report.blend_equation), int(dyn_report.write_mask), unsigned(options.dynamic_state),
      int((dyn_mask & dyn::kRaster) != 0), int((dyn_mask & dyn::kBlend) != 0));

  // ---- what the device offers ----
  caps_.depth_format = PickDepthFormat(physical_);
  caps_.bc = enabled.textureCompressionBC;
  caps_.astc_ldr = enabled.textureCompressionASTC_LDR;
  caps_.depth_clamp = enabled.depthClamp;
  caps_.occlusion_precise = enabled.occlusionQueryPrecise != 0;  // S102 G11
  caps_.multiview = multiview_enabled.multiview != 0;  // S102 C1
  caps_.anisotropy = enabled.samplerAnisotropy;
  caps_.max_anisotropy = enabled.samplerAnisotropy ? properties_.limits.maxSamplerAnisotropy : 1.0f;
  caps_.uniform_align = properties_.limits.minUniformBufferOffsetAlignment;
  caps_.atom_size = properties_.limits.nonCoherentAtomSize;
  caps_.unified_memory = properties_.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
    if (memory_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) caps_.lazy_memory = true;
  }
  Log(RDR_LOG_DEVICE_FMT, properties_.deviceName, VK_API_VERSION_MAJOR(properties_.apiVersion),
      VK_API_VERSION_MINOR(properties_.apiVersion), VK_API_VERSION_PATCH(properties_.apiVersion), properties_.driverVersion, caps_.unified_memory, caps_.lazy_memory, caps_.bc,
      caps_.astc_ldr);
  return true;
}

void Device::Destroy() {
  if (device_) vk::vkDestroyDevice(device_, nullptr);
  device_ = VK_NULL_HANDLE;
  if (messenger_) vk::vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
  messenger_ = VK_NULL_HANDLE;
  if (instance_) vk::vkDestroyInstance(instance_, nullptr);
  instance_ = VK_NULL_HANDLE;
}

int Device::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const {
  int fallback = -1;
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
    if (!(type_bits & (1u << i))) continue;
    const VkMemoryPropertyFlags flags = memory_.memoryTypes[i].propertyFlags;
    if ((flags & required) != required) continue;
    if ((flags & preferred) == preferred) return int(i);
    if (fallback < 0) fallback = int(i);
  }
  return fallback;
}

VkFormatFeatureFlags Device::FormatFeatures(VkFormat format) const {
  VkFormatProperties props{};
  vk::vkGetPhysicalDeviceFormatProperties(physical_, format, &props);
  return ApplyFormatCapMask(format, props.optimalTilingFeatures, format_caps_mask_);
}

bool Device::VertexFormatReadable(VkFormat format) const {
  VkFormatProperties props{};
  vk::vkGetPhysicalDeviceFormatProperties(physical_, format, &props);
  return (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}

bool Device::FormatHas(VkFormat format, VkFormatFeatureFlags features) const {
  return (FormatFeatures(format) & features) == features;
}

}  // namespace rdr
