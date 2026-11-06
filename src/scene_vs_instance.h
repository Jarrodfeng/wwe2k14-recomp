#pragma once
// Session 94 (idea 4, gpu_native_instance_world): the instancing variants of our arena vertex shaders (src\scene\spirv\*_inst_vert.h, built by
// tools/shaders/build_instanced_vs.py: every float constant read at register + gl_InstanceIndex * stride). Registered beside the plain shaders by
// scene_vs_swap::Install() when the SDK switch gpu_native_instance_world is on; the SDK takes a variant only for the draws vulkan_repeat_draw_merge
// merges (back-to-back draws that differ only in their vertex constants, the world matrix c76-c78), so a draw alone keeps its plain shader.
#include <cstddef>
#include <cstdint>
#include <iterator>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "scene/spirv/arena_vs_bg_vert.h"
#include "scene/spirv/arena_vs_bg_inst_vert.h"
#include "scene/spirv/arena_vs_bg_bump_vert.h"
#include "scene/spirv/arena_vs_bg_bump_inst_vert.h"
#include "scene/spirv/arena_vs_bg_lm_vert.h"
#include "scene/spirv/arena_vs_bg_lm_inst_vert.h"
#include "scene/spirv/arena_vs_bg_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_lm_tf_inst_vert.h"
#include "scene/spirv/arena_vs_bg_rim_vert.h"
#include "scene/spirv/arena_vs_bg_rim_inst_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_inst_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_inst_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_lm_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_lm_inst_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_lm_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_lm_inst_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_inst_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_inst_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_tf_inst_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_plain_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_plain_inst_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_spec_lm_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_spec_lm_inst_vert.h"
#include "scene/spirv/arena_vs_bg_spec_vert.h"
#include "scene/spirv/arena_vs_bg_spec_inst_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_inst_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_lm_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_lm_inst_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_inst_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_tf_inst_vert.h"
#include "scene/spirv/arena_vs_bg_spec_tf_vert.h"
#include "scene/spirv/arena_vs_bg_spec_tf_inst_vert.h"
#include "scene/spirv/arena_vs_bg_tf_vert.h"
#include "scene/spirv/arena_vs_bg_tf_inst_vert.h"
#include "scene/spirv/arena_vs_particle_vert.h"
#include "scene/spirv/arena_vs_particle_inst_vert.h"
#include "scene/spirv/arena_vs_prop_vert.h"
#include "scene/spirv/arena_vs_prop_inst_vert.h"
#include "scene/spirv/arena_vs_prop_bump_vert.h"
#include "scene/spirv/arena_vs_prop_bump_inst_vert.h"

namespace wwe2k14::scene_vs_instance {

struct Variant {
  const uint32_t* plain;  // the plain shader's SPIR-V (the key: scene_vs_swap's rows point at it)
  const uint32_t* inst;
  size_t words;
};
inline const Variant kVariants[] = {
    {arena_vs_bg_vert_spv, arena_vs_bg_inst_vert_spv, std::size(arena_vs_bg_inst_vert_spv)},
    {arena_vs_bg_bump_vert_spv, arena_vs_bg_bump_inst_vert_spv, std::size(arena_vs_bg_bump_inst_vert_spv)},
    {arena_vs_bg_lm_vert_spv, arena_vs_bg_lm_inst_vert_spv, std::size(arena_vs_bg_lm_inst_vert_spv)},
    {arena_vs_bg_lm_tf_vert_spv, arena_vs_bg_lm_tf_inst_vert_spv, std::size(arena_vs_bg_lm_tf_inst_vert_spv)},
    {arena_vs_bg_rim_vert_spv, arena_vs_bg_rim_inst_vert_spv, std::size(arena_vs_bg_rim_inst_vert_spv)},
    {arena_vs_bg_scroll_vert_spv, arena_vs_bg_scroll_inst_vert_spv, std::size(arena_vs_bg_scroll_inst_vert_spv)},
    {arena_vs_bg_scroll2_vert_spv, arena_vs_bg_scroll2_inst_vert_spv, std::size(arena_vs_bg_scroll2_inst_vert_spv)},
    {arena_vs_bg_scroll2_lm_vert_spv, arena_vs_bg_scroll2_lm_inst_vert_spv, std::size(arena_vs_bg_scroll2_lm_inst_vert_spv)},
    {arena_vs_bg_scroll_lm_vert_spv, arena_vs_bg_scroll_lm_inst_vert_spv, std::size(arena_vs_bg_scroll_lm_inst_vert_spv)},
    {arena_vs_bg_shadow_vert_spv, arena_vs_bg_shadow_inst_vert_spv, std::size(arena_vs_bg_shadow_inst_vert_spv)},
    {arena_vs_bg_shadow_lm_vert_spv, arena_vs_bg_shadow_lm_inst_vert_spv, std::size(arena_vs_bg_shadow_lm_inst_vert_spv)},
    {arena_vs_bg_shadow_lm_tf_vert_spv, arena_vs_bg_shadow_lm_tf_inst_vert_spv, std::size(arena_vs_bg_shadow_lm_tf_inst_vert_spv)},
    {arena_vs_bg_shadow_plain_vert_spv, arena_vs_bg_shadow_plain_inst_vert_spv, std::size(arena_vs_bg_shadow_plain_inst_vert_spv)},
    {arena_vs_bg_shadow_spec_lm_vert_spv, arena_vs_bg_shadow_spec_lm_inst_vert_spv, std::size(arena_vs_bg_shadow_spec_lm_inst_vert_spv)},
    {arena_vs_bg_spec_vert_spv, arena_vs_bg_spec_inst_vert_spv, std::size(arena_vs_bg_spec_inst_vert_spv)},
    {arena_vs_bg_spec_fade_vert_spv, arena_vs_bg_spec_fade_inst_vert_spv, std::size(arena_vs_bg_spec_fade_inst_vert_spv)},
    {arena_vs_bg_spec_fade_lm_vert_spv, arena_vs_bg_spec_fade_lm_inst_vert_spv, std::size(arena_vs_bg_spec_fade_lm_inst_vert_spv)},
    {arena_vs_bg_spec_lm_vert_spv, arena_vs_bg_spec_lm_inst_vert_spv, std::size(arena_vs_bg_spec_lm_inst_vert_spv)},
    {arena_vs_bg_spec_lm_tf_vert_spv, arena_vs_bg_spec_lm_tf_inst_vert_spv, std::size(arena_vs_bg_spec_lm_tf_inst_vert_spv)},
    {arena_vs_bg_spec_tf_vert_spv, arena_vs_bg_spec_tf_inst_vert_spv, std::size(arena_vs_bg_spec_tf_inst_vert_spv)},
    {arena_vs_bg_tf_vert_spv, arena_vs_bg_tf_inst_vert_spv, std::size(arena_vs_bg_tf_inst_vert_spv)},
    {arena_vs_particle_vert_spv, arena_vs_particle_inst_vert_spv, std::size(arena_vs_particle_inst_vert_spv)},
    {arena_vs_prop_vert_spv, arena_vs_prop_inst_vert_spv, std::size(arena_vs_prop_inst_vert_spv)},
    {arena_vs_prop_bump_vert_spv, arena_vs_prop_bump_inst_vert_spv, std::size(arena_vs_prop_bump_inst_vert_spv)},
};

// `mats` = scene_vs_swap::kArenaVertexShaders (any row type with hash / spv); after they are registered. SDK flag bit 30 of
// RegisterNativeVertexSwap = "this is the instancing variant of the hash" (VulkanCommandProcessor::RegisterNativeVertexSwap).
template <class Cp, class Mat>
inline int RegisterAll(Cp* cp, const Mat* mats, size_t count) {
  if (rex::cvar::GetFlagByName("gpu_native_instance_world") != "true") return 0;
  int ok = 0;
  for (size_t i = 0; i < count; ++i) {
    for (const Variant& v : kVariants) {
      if (v.plain == mats[i].spv) {
        ok += cp->RegisterNativeVertexSwap(mats[i].hash, v.inst, v.words, nullptr, 0, nullptr, 0x40000000u, 0, 0) ? 1 : 0;
        break;
      }
    }
  }
  REXLOG_INFO("[native] world instancing: {} instancing variants registered", ok);
  return ok;
}

}  // namespace wwe2k14::scene_vs_instance
