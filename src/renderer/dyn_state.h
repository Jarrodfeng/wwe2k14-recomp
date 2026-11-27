// S111 DYNSTATE: draw state the device can set per draw leaves the pipeline key (docs\renderer\DYNAMIC_STATE.md).
// entry: rdr::dyn::MaskOf, rdr::dyn::KeyState, rdr::dyn::Apply
//
// A pipeline used to bake every byte of DrawState, so a draw whose blend, depth write or cull changed was another pipeline: a build on the render thread at
// its first draw, and a shipped pipeline seed that went stale with every change to how something is drawn. Where the device offers it, those bytes are now set
// by a command at record time (as the game's own console did: render states per draw, no pipeline objects), and the key holds what is left.
//
// Two groups, each all-or-nothing on a device:
//   kRaster  cull mode, front face, depth test / write / compare   VK_EXT_extended_dynamic_state (one feature bit; core in Vulkan 1.3, but the renderer's
//            instance asks for 1.1, so the extension's entry points are the ones used)
//   kBlend   per colour attachment: blend enable, blend equation, colour write mask   VK_EXT_extended_dynamic_state3 (three feature bits, all three needed)
// The device's mask is decided ONCE, at device creation (Device::Create -> Caps::dyn_mask), and never changes: every pipeline of a session has the same
// dynamic set, so binding one never unsets what a draw before it set. A group the device lacks stays in the key exactly as before.
//
// No Vulkan in this file (the unit tools\renderer\dyn_state_unit.cpp builds it alone); the commands are behind a sink (dyn_state_vk.h).
#pragma once
#include <cstdint>
#include <cstring>

#include "api.h"

namespace rdr::dyn {

enum Group : uint32_t {
  kRaster = 1u << 0,
  kBlend = 1u << 1,
  kAll = kRaster | kBlend,
};

// What the device answered (Device::Create fills it and logs it).
struct Report {
  bool ext1 = false, feature1 = false;                                                   // VK_EXT_extended_dynamic_state listed / extendedDynamicState
  bool ext3 = false, blend_enable = false, blend_equation = false, write_mask = false;   // VK_EXT_extended_dynamic_state3 listed / its three colour blend bits
};

// The groups in use: what the device has, and the caller allows (RendererOptions::dynamic_state; a harvest of the pipeline seed allows none).
inline uint32_t MaskOf(const Report& r, uint32_t allowed) {
  uint32_t m = 0;
  if (r.ext1 && r.feature1) m |= kRaster;
  if (r.ext3 && r.blend_enable && r.blend_equation && r.write_mask) m |= kBlend;
  return m & allowed;
}

// The state a pipeline is keyed by and built from: the bytes of a group in use are zero (the pipeline does not bake them). Everything else is kept:
// program, vertex layout, topology, depth clamp, stencil test and faces, depth bias enable, primitive restart, variant.
inline DrawState KeyState(const DrawState& s, uint32_t mask) {
  DrawState k = s;
  if (mask & kRaster) k.cull = 0, k.front_face = 0, k.depth_test = 0, k.depth_write = 0, k.depth_compare = 0;
  if (mask & kBlend) std::memset(k.blend, 0, sizeof(k.blend));
  return k;
}

// A blend that is a plain write (ONE, ZERO, ADD on colour and alpha) needs no blend feature of the attachment's format (the same rule BuildPipeline applies
// to a baked blend). The numbers are VK_BLEND_FACTOR_ONE = 1, VK_BLEND_FACTOR_ZERO = 0, VK_BLEND_OP_ADD = 0.
inline bool PlainWrite(const BlendState& b) {
  return b.src_color == 1 && b.dst_color == 0 && b.color_op == 0 && b.src_alpha == 1 && b.dst_alpha == 0 && b.alpha_op == 0;
}

// What was last set in this render pass. A fresh one (nothing set) starts every render pass: the first draw sets everything in use.
struct Applied {
  bool raster = false;
  uint8_t cull = 0, front_face = 0, depth_test = 0, depth_write = 0, depth_compare = 0;
  uint8_t blend_set = 0;   // bit c: attachment c's enable, equation and write mask were set
  BlendState blend[kMaxColorTargets] = {};
};

struct Counts {
  uint32_t sets = 0;            // commands issued
  uint32_t blend_masked = 0;    // a blend switched off because the attachment's format cannot blend (format caps, fallbacks on)
  uint32_t blend_illegal = 0;   // ... asked of the driver anyway (fallbacks off)
};

// Sets the state of `s` that the pipeline no longer bakes, leaving out what already holds that value. Called for EVERY draw after its pipeline is bound:
// two draws on one pipeline can differ in exactly these bytes. `blend_ok` bit c = colour attachment c's format can blend.
// Sink: CullMode(uint8_t), FrontFace(uint8_t), DepthTest(bool), DepthWrite(bool), DepthCompare(uint8_t), BlendEnable(uint32_t c, bool),
// BlendEquation(uint32_t c, const BlendState&), WriteMask(uint32_t c, uint8_t).
template <class Sink>
inline void Apply(Sink& sink, Applied& last, const DrawState& s, uint32_t mask, uint32_t color_count, uint32_t blend_ok, bool fallback, Counts& n) {
  if (mask & kRaster) {
    const bool all = !last.raster;
    const uint8_t dt = s.depth_test ? 1 : 0, dw = s.depth_write ? 1 : 0;
    if (all || last.cull != s.cull) sink.CullMode(s.cull), last.cull = s.cull, ++n.sets;
    if (all || last.front_face != s.front_face) sink.FrontFace(s.front_face), last.front_face = s.front_face, ++n.sets;
    if (all || last.depth_test != dt) sink.DepthTest(dt != 0), last.depth_test = dt, ++n.sets;
    if (all || last.depth_write != dw) sink.DepthWrite(dw != 0), last.depth_write = dw, ++n.sets;
    if (all || last.depth_compare != s.depth_compare) sink.DepthCompare(s.depth_compare), last.depth_compare = s.depth_compare, ++n.sets;
    last.raster = true;
  }
  if (mask & kBlend) {
    const uint32_t count = color_count < uint32_t(kMaxColorTargets) ? color_count : uint32_t(kMaxColorTargets);
    for (uint32_t c = 0; c < count; ++c) {
      BlendState b = s.blend[c];
      b.enable = b.enable ? 1 : 0;
      if (b.enable && !PlainWrite(b) && !(blend_ok & (1u << c))) {
        if (fallback) b.enable = 0, ++n.blend_masked;
        else ++n.blend_illegal;
      }
      const bool all = !(last.blend_set & (1u << c));
      const BlendState& o = last.blend[c];
      if (all || o.enable != b.enable) sink.BlendEnable(c, b.enable != 0), ++n.sets;
      if (all || o.src_color != b.src_color || o.dst_color != b.dst_color || o.color_op != b.color_op || o.src_alpha != b.src_alpha || o.dst_alpha != b.dst_alpha || o.alpha_op != b.alpha_op)
        sink.BlendEquation(c, b), ++n.sets;
      if (all || o.write_mask != b.write_mask) sink.WriteMask(c, b.write_mask), ++n.sets;
      last.blend[c] = b;
      last.blend_set = uint8_t(last.blend_set | (1u << c));
    }
  }
}

}  // namespace rdr::dyn
