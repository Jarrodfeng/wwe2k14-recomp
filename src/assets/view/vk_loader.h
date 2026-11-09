// Session 98 (Arena Viewer phone port, stage B): the Vulkan entry points the viewer uses, loaded at run time from the system
// loader (vulkan-1.dll / libvulkan.so) so the exe starts without Vulkan and the phone build links nothing extra.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <string>

#define VKV_GLOBAL_FNS(X)                     \
  X(vkCreateInstance)                         \
  X(vkEnumerateInstanceExtensionProperties)   \
  X(vkEnumerateInstanceLayerProperties)

#define VKV_INSTANCE_FNS(X)                        \
  X(vkDestroyInstance)                             \
  X(vkEnumeratePhysicalDevices)                    \
  X(vkGetPhysicalDeviceProperties)                 \
  X(vkGetPhysicalDeviceFeatures)                   \
  X(vkGetPhysicalDeviceQueueFamilyProperties)      \
  X(vkGetPhysicalDeviceMemoryProperties)           \
  X(vkGetPhysicalDeviceFormatProperties)           \
  X(vkCreateDevice)                                \
  X(vkGetDeviceProcAddr)                           \
  X(vkEnumerateDeviceExtensionProperties)          \
  X(vkDestroySurfaceKHR)                           \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)          \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)     \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)          \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VKV_DEVICE_FNS(X)              \
  X(vkDestroyDevice)                   \
  X(vkGetDeviceQueue)                  \
  X(vkDeviceWaitIdle)                  \
  X(vkQueueSubmit)                     \
  X(vkQueueWaitIdle)                   \
  X(vkAllocateMemory)                  \
  X(vkFreeMemory)                      \
  X(vkMapMemory)                       \
  X(vkUnmapMemory)                     \
  X(vkInvalidateMappedMemoryRanges)    \
  X(vkCreateBuffer)                    \
  X(vkDestroyBuffer)                   \
  X(vkGetBufferMemoryRequirements)     \
  X(vkBindBufferMemory)                \
  X(vkCreateImage)                     \
  X(vkDestroyImage)                    \
  X(vkGetImageMemoryRequirements)      \
  X(vkBindImageMemory)                 \
  X(vkCreateImageView)                 \
  X(vkDestroyImageView)                \
  X(vkCreateSampler)                   \
  X(vkDestroySampler)                  \
  X(vkCreateShaderModule)              \
  X(vkDestroyShaderModule)             \
  X(vkCreateDescriptorSetLayout)       \
  X(vkDestroyDescriptorSetLayout)      \
  X(vkCreatePipelineLayout)            \
  X(vkDestroyPipelineLayout)           \
  X(vkCreateGraphicsPipelines)         \
  X(vkDestroyPipeline)                 \
  X(vkCreateRenderPass)                \
  X(vkDestroyRenderPass)               \
  X(vkCreateFramebuffer)               \
  X(vkDestroyFramebuffer)              \
  X(vkCreateDescriptorPool)            \
  X(vkDestroyDescriptorPool)           \
  X(vkResetDescriptorPool)             \
  X(vkAllocateDescriptorSets)          \
  X(vkUpdateDescriptorSets)            \
  X(vkCreateCommandPool)               \
  X(vkDestroyCommandPool)              \
  X(vkAllocateCommandBuffers)          \
  X(vkFreeCommandBuffers)              \
  X(vkBeginCommandBuffer)              \
  X(vkEndCommandBuffer)                \
  X(vkResetCommandBuffer)              \
  X(vkCreateFence)                     \
  X(vkDestroyFence)                    \
  X(vkWaitForFences)                   \
  X(vkResetFences)                     \
  X(vkCreateSemaphore)                 \
  X(vkDestroySemaphore)                \
  X(vkCreateQueryPool)                 \
  X(vkDestroyQueryPool)                \
  X(vkGetQueryPoolResults)             \
  X(vkCmdResetQueryPool)               \
  X(vkCmdWriteTimestamp)               \
  X(vkCmdBeginRenderPass)              \
  X(vkCmdEndRenderPass)                \
  X(vkCmdBindPipeline)                 \
  X(vkCmdBindDescriptorSets)           \
  X(vkCmdBindVertexBuffers)            \
  X(vkCmdBindIndexBuffer)              \
  X(vkCmdDraw)                         \
  X(vkCmdDrawIndexed)                  \
  X(vkCmdSetViewport)                  \
  X(vkCmdSetScissor)                   \
  X(vkCmdPipelineBarrier)              \
  X(vkCmdCopyBuffer)                   \
  X(vkCmdCopyBufferToImage)            \
  X(vkCmdCopyImageToBuffer)            \
  X(vkCmdCopyImage)                    \
  X(vkCmdBlitImage)                    \
  X(vkCmdClearColorImage)              \
  X(vkCreateSwapchainKHR)              \
  X(vkDestroySwapchainKHR)             \
  X(vkGetSwapchainImagesKHR)           \
  X(vkAcquireNextImageKHR)             \
  X(vkQueuePresentKHR)

namespace vkfn {
#define VKV_DECL(n) extern PFN_##n n;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VKV_GLOBAL_FNS(VKV_DECL)
VKV_INSTANCE_FNS(VKV_DECL)
VKV_DEVICE_FNS(VKV_DECL)
#undef VKV_DECL

bool LoadLoader(std::string& err);           // the system loader + the global functions
void LoadInstance(VkInstance instance);       // instance-level functions (missing optional ones stay null)
void LoadDevice(VkDevice device);             // device-level functions
PFN_vkVoidFunction GetProc(const char* name, void* instance);  // for imgui_impl_vulkan's loader
}  // namespace vkfn
