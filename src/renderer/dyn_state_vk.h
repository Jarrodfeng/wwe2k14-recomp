// S111 DYNSTATE: the Vulkan side of dyn_state.h: the commands that set what a pipeline no longer bakes.
// entry: rdr::dyn::VkSink
// The entry points are the extensions' (VK_EXT_extended_dynamic_state, VK_EXT_extended_dynamic_state3), loaded with the device (vk_api.h RDR_DEVICE_FNS);
// a pointer is null on a device without its extension, and Device::Create leaves that group out of Caps::dyn_mask, so dyn::Apply never calls it.
#pragma once
#include "dyn_state.h"
#include "vk_api.h"

namespace rdr::dyn {

struct VkSink {
  VkCommandBuffer cmd;
  void CullMode(uint8_t v) { vk::vkCmdSetCullModeEXT(cmd, VkCullModeFlags(v)); }
  void FrontFace(uint8_t v) { vk::vkCmdSetFrontFaceEXT(cmd, VkFrontFace(v)); }
  void DepthTest(bool on) { vk::vkCmdSetDepthTestEnableEXT(cmd, on ? VK_TRUE : VK_FALSE); }
  void DepthWrite(bool on) { vk::vkCmdSetDepthWriteEnableEXT(cmd, on ? VK_TRUE : VK_FALSE); }
  void DepthCompare(uint8_t v) { vk::vkCmdSetDepthCompareOpEXT(cmd, VkCompareOp(v)); }
  void BlendEnable(uint32_t c, bool on) {
    const VkBool32 enable = on ? VK_TRUE : VK_FALSE;
    vk::vkCmdSetColorBlendEnableEXT(cmd, c, 1, &enable);
  }
  void BlendEquation(uint32_t c, const BlendState& b) {
    VkColorBlendEquationEXT e{};
    e.srcColorBlendFactor = VkBlendFactor(b.src_color), e.dstColorBlendFactor = VkBlendFactor(b.dst_color), e.colorBlendOp = VkBlendOp(b.color_op);
    e.srcAlphaBlendFactor = VkBlendFactor(b.src_alpha), e.dstAlphaBlendFactor = VkBlendFactor(b.dst_alpha), e.alphaBlendOp = VkBlendOp(b.alpha_op);
    vk::vkCmdSetColorBlendEquationEXT(cmd, c, 1, &e);
  }
  void WriteMask(uint32_t c, uint8_t m) {
    const VkColorComponentFlags mask = m;
    vk::vkCmdSetColorWriteMaskEXT(cmd, c, 1, &mask);
  }
};

}  // namespace rdr::dyn
