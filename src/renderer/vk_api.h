// The Vulkan entry points the renderer uses, loaded at run time so the program starts without Vulkan and links nothing extra.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
// No platform defines here: they would pull windows.h and its macros into every file. The swapchain code loads its surface call itself.
#include <vulkan/vulkan.h>

#define RDR_GLOBAL_FNS(X)                   \
  X(vkCreateInstance)                       \
  X(vkEnumerateInstanceVersion)             \
  X(vkEnumerateInstanceExtensionProperties) \
  X(vkEnumerateInstanceLayerProperties)

#define RDR_INSTANCE_FNS(X)                   \
  X(vkDestroyInstance)                        \
  X(vkEnumeratePhysicalDevices)               \
  X(vkGetPhysicalDeviceProperties)            \
  X(vkGetPhysicalDeviceFeatures)              \
  X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkGetPhysicalDeviceMemoryProperties)      \
  X(vkGetPhysicalDeviceFormatProperties)      \
  X(vkEnumerateDeviceExtensionProperties)     \
  X(vkCreateDevice)                           \
  X(vkGetDeviceProcAddr)                      \
  X(vkDestroySurfaceKHR)                      \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)     \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)\
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)     \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)\
  X(vkCreateDebugUtilsMessengerEXT)           \
  X(vkDestroyDebugUtilsMessengerEXT)          \
  X(vkSetDebugUtilsObjectNameEXT)

#define RDR_DEVICE_FNS(X)            \
  X(vkDestroyDevice)                 \
  X(vkGetRefreshCycleDurationGOOGLE) \
  X(vkGetPastPresentationTimingGOOGLE) \
  X(vkGetDeviceQueue)                \
  X(vkDeviceWaitIdle)                \
  X(vkQueueSubmit)                   \
  X(vkQueueWaitIdle)                 \
  X(vkAllocateMemory)                \
  X(vkFreeMemory)                    \
  X(vkMapMemory)                     \
  X(vkUnmapMemory)                   \
  X(vkFlushMappedMemoryRanges)       \
  X(vkInvalidateMappedMemoryRanges)  \
  X(vkCreateBuffer)                  \
  X(vkDestroyBuffer)                 \
  X(vkGetBufferMemoryRequirements)   \
  X(vkBindBufferMemory)              \
  X(vkCreateImage)                   \
  X(vkDestroyImage)                  \
  X(vkGetImageMemoryRequirements)    \
  X(vkBindImageMemory)               \
  X(vkCreateImageView)               \
  X(vkDestroyImageView)              \
  X(vkCreateSampler)                 \
  X(vkDestroySampler)                \
  X(vkCreateShaderModule)            \
  X(vkDestroyShaderModule)           \
  X(vkCreatePipelineCache)           \
  X(vkDestroyPipelineCache)          \
  X(vkGetPipelineCacheData)          \
  X(vkCreateGraphicsPipelines)       \
  X(vkDestroyPipeline)               \
  X(vkCreatePipelineLayout)          \
  X(vkDestroyPipelineLayout)         \
  X(vkCreateDescriptorSetLayout)     \
  X(vkDestroyDescriptorSetLayout)    \
  X(vkCreateDescriptorPool)          \
  X(vkDestroyDescriptorPool)         \
  X(vkResetDescriptorPool)           \
  X(vkAllocateDescriptorSets)        \
  X(vkUpdateDescriptorSets)          \
  X(vkCreateRenderPass)              \
  X(vkDestroyRenderPass)             \
  X(vkCreateFramebuffer)             \
  X(vkDestroyFramebuffer)            \
  X(vkCreateCommandPool)             \
  X(vkDestroyCommandPool)            \
  X(vkResetCommandPool)              \
  X(vkAllocateCommandBuffers)        \
  X(vkBeginCommandBuffer)            \
  X(vkEndCommandBuffer)              \
  X(vkCreateFence)                   \
  X(vkDestroyFence)                  \
  X(vkResetFences)                   \
  X(vkWaitForFences)                 \
  X(vkGetFenceStatus)                \
  X(vkCreateSemaphore)               \
  X(vkDestroySemaphore)              \
  X(vkCmdPipelineBarrier)            \
  X(vkCmdBeginRenderPass)            \
  X(vkCmdEndRenderPass)              \
  X(vkCmdBindPipeline)               \
  X(vkCmdBindDescriptorSets)         \
  X(vkCmdBindVertexBuffers)          \
  X(vkCmdBindIndexBuffer)            \
  X(vkCmdPushConstants)              \
  X(vkCmdSetViewport)                \
  X(vkCmdSetScissor)                 \
  X(vkCmdSetDepthBias)               \
  X(vkCmdSetBlendConstants)          \
  X(vkCmdSetStencilCompareMask)      \
  X(vkCmdSetStencilWriteMask)        \
  X(vkCmdSetStencilReference)        \
  X(vkCmdDraw)                       \
  X(vkCmdDrawIndexed)                \
  X(vkCmdClearAttachments)           \
  X(vkCmdClearColorImage)            \
  X(vkCmdClearDepthStencilImage)     \
  X(vkCmdCopyBuffer)                 \
  X(vkCmdCopyImage)                  \
  X(vkCmdBlitImage)                  \
  X(vkCmdCopyBufferToImage)          \
  X(vkCmdCopyImageToBuffer)          \
  X(vkCreateQueryPool)               \
  X(vkDestroyQueryPool)              \
  X(vkCmdResetQueryPool)             \
  X(vkCmdWriteTimestamp)             \
  X(vkGetQueryPoolResults)           \
  X(vkCreateSwapchainKHR)            \
  X(vkDestroySwapchainKHR)           \
  X(vkGetSwapchainImagesKHR)         \
  X(vkAcquireNextImageKHR)           \
  X(vkQueuePresentKHR)

#define RDR_DECLARE(name) extern PFN_##name name;
namespace rdr::vk {
RDR_GLOBAL_FNS(RDR_DECLARE)
RDR_INSTANCE_FNS(RDR_DECLARE)
RDR_DEVICE_FNS(RDR_DECLARE)

// Loads the system Vulkan library and the global entry points. False if the system has no Vulkan.
bool LoadVulkan();
// Settings parity S10b: every call through this vkGetInstanceProcAddr instead of the loader's (Streamline's interposer, streamline.h).
// Before LoadVulkan; null = the loader's.
void SetInstanceProcAddrOverride(PFN_vkGetInstanceProcAddr proc);
// Lane 256 (Android, guest_gpu_only: no SDK Vulkan instance opens the bundled custom driver any more): LoadVulkan asks this first; a library
// handle (from libadrenotools) is searched for vkGetInstanceProcAddr; null, or no opener = the system's libvulkan. Before LoadVulkan. Not used on Windows.
void SetLoaderOpener(void* (*open_library)());
void* GetInstanceProc(VkInstance instance, const char* name);
void LoadInstance(VkInstance instance);
void LoadDevice(VkDevice device);
}  // namespace rdr::vk
#undef RDR_DECLARE
