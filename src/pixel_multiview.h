#pragma once
// Session 95 (gpu_d3d_decouple phase 2, item F1): the layer-true (multiview) variants of our pixel shaders that read a target drawn in a multiview
// pass (tools/shaders/build_multiview_ps.py): the post combines and the side chain. Registered next to the plain shader ONLY with gpu_d3d_decouple on
// (pattern: scene_vs_multiview.h); with it off nothing here is touched and the registrations are byte for byte the old ones.
// A shader registered with kLayerTrue and no variant is refused by the SDK (shader_swap.cpp Build), never read at layer 0.

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

// (the plain headers too: the table maps each plain array to its variant)
#include "post/spirv/entrance_combine_enhanced.h"
#include "post/spirv/entrance_combine_original_mp.h"
#include "post/spirv/entrance_combine_original.h"
#include "post/spirv/entrance_combine_lite.h"
#include "post/spirv/entrance_combine_probe.h"
#include "post/spirv/match_combine_enhanced.h"
#include "post/spirv/match_combine_original_mp.h"
#include "post/spirv/match_combine_original.h"
#include "post/spirv/match_combine_lite.h"
#include "post/spirv/match_combine_probe.h"
#include "scene/spirv/audience_copy.h"
#include "scene/spirv/liquid_map_copy.h"
#include "scene/spirv/side_bloom_bright4.h"
#include "scene/spirv/side_blur10.h"
#include "scene/spirv/side_blur13.h"
#include "scene/spirv/side_blur15.h"
#include "scene/spirv/side_blur21.h"
#include "scene/spirv/side_blur4_mask_inv.h"
#include "scene/spirv/side_blur4_mask.h"
#include "scene/spirv/side_blur5.h"
#include "scene/spirv/side_dof_blur.h"
#include "scene/spirv/side_dof_downsample.h"
#include "scene/spirv/side_downscale4.h"
#include "scene/spirv/side_glare_blur8.h"
#include "scene/spirv/side_glare_combine.h"
#include "scene/spirv/side_liquid_layer.h"
#include "post/spirv/entrance_combine_enhanced_mv.h"
#include "post/spirv/entrance_combine_original_mp_mv.h"
#include "post/spirv/entrance_combine_original_mv.h"
#include "post/spirv/entrance_combine_lite_mv.h"
#include "post/spirv/entrance_combine_probe_mv.h"
#include "post/spirv/match_combine_enhanced_mv.h"
#include "post/spirv/match_combine_original_mp_mv.h"
#include "post/spirv/match_combine_original_mv.h"
#include "post/spirv/match_combine_lite_mv.h"
#include "post/spirv/match_combine_probe_mv.h"
#include "scene/spirv/audience_copy_mv.h"
#include "scene/spirv/liquid_map_copy_mv.h"
#include "scene/spirv/side_bloom_bright4_mv.h"
#include "scene/spirv/side_blur10_mv.h"
#include "scene/spirv/side_blur13_mv.h"
#include "scene/spirv/side_blur15_mv.h"
#include "scene/spirv/side_blur21_mv.h"
#include "scene/spirv/side_blur4_mask_inv_mv.h"
#include "scene/spirv/side_blur4_mask_mv.h"
#include "scene/spirv/side_blur5_mv.h"
#include "scene/spirv/side_dof_blur_mv.h"
#include "scene/spirv/side_dof_downsample_mv.h"
#include "scene/spirv/side_downscale4_mv.h"
#include "scene/spirv/side_glare_blur8_mv.h"
#include "scene/spirv/side_glare_combine_mv.h"
#include "scene/spirv/side_liquid_layer_mv.h"

namespace wwe2k14::pixel_multiview {

// SDK shader_swap::kFlagLayerTrue / the "this is the multiview variant" flag of RegisterNativeShaderSwap.
inline constexpr uint32_t kLayerTrue = 16;
inline constexpr uint32_t kVariantFlag = 0x20000000u;

struct Variant {
  const uint32_t* plain;
  const uint32_t* mv;
  size_t words;
};
inline constexpr Variant kVariants[] = {
    {entrance_combine_enhanced_spv, entrance_combine_enhanced_mv_spv, std::size(entrance_combine_enhanced_mv_spv)},
    {entrance_combine_original_mp_spv, entrance_combine_original_mp_mv_spv, std::size(entrance_combine_original_mp_mv_spv)},
    {entrance_combine_original_spv, entrance_combine_original_mv_spv, std::size(entrance_combine_original_mv_spv)},
    {entrance_combine_probe_spv, entrance_combine_probe_mv_spv, std::size(entrance_combine_probe_mv_spv)},
    {match_combine_enhanced_spv, match_combine_enhanced_mv_spv, std::size(match_combine_enhanced_mv_spv)},
    {match_combine_original_mp_spv, match_combine_original_mp_mv_spv, std::size(match_combine_original_mp_mv_spv)},
    {match_combine_original_spv, match_combine_original_mv_spv, std::size(match_combine_original_mv_spv)},
    {match_combine_probe_spv, match_combine_probe_mv_spv, std::size(match_combine_probe_mv_spv)},
    {entrance_combine_lite_spv, entrance_combine_lite_mv_spv, std::size(entrance_combine_lite_mv_spv)},
    {match_combine_lite_spv, match_combine_lite_mv_spv, std::size(match_combine_lite_mv_spv)},
    {audience_copy_spv, audience_copy_mv_spv, std::size(audience_copy_mv_spv)},
    {liquid_map_copy_spv, liquid_map_copy_mv_spv, std::size(liquid_map_copy_mv_spv)},
    {side_bloom_bright4_spv, side_bloom_bright4_mv_spv, std::size(side_bloom_bright4_mv_spv)},
    {side_blur10_spv, side_blur10_mv_spv, std::size(side_blur10_mv_spv)},
    {side_blur13_spv, side_blur13_mv_spv, std::size(side_blur13_mv_spv)},
    {side_blur15_spv, side_blur15_mv_spv, std::size(side_blur15_mv_spv)},
    {side_blur21_spv, side_blur21_mv_spv, std::size(side_blur21_mv_spv)},
    {side_blur4_mask_inv_spv, side_blur4_mask_inv_mv_spv, std::size(side_blur4_mask_inv_mv_spv)},
    {side_blur4_mask_spv, side_blur4_mask_mv_spv, std::size(side_blur4_mask_mv_spv)},
    {side_blur5_spv, side_blur5_mv_spv, std::size(side_blur5_mv_spv)},
    {side_dof_blur_spv, side_dof_blur_mv_spv, std::size(side_dof_blur_mv_spv)},
    {side_dof_downsample_spv, side_dof_downsample_mv_spv, std::size(side_dof_downsample_mv_spv)},
    {side_downscale4_spv, side_downscale4_mv_spv, std::size(side_downscale4_mv_spv)},
    {side_glare_blur8_spv, side_glare_blur8_mv_spv, std::size(side_glare_blur8_mv_spv)},
    {side_glare_combine_spv, side_glare_combine_mv_spv, std::size(side_glare_combine_mv_spv)},
    {side_liquid_layer_spv, side_liquid_layer_mv_spv, std::size(side_liquid_layer_mv_spv)},
};

inline bool On() {
  static const bool on = [] {
    try {
      return std::stoi(rex::cvar::GetFlagByName("gpu_d3d_decouple")) != 0;
    } catch (...) {
      return false;
    }
  }();
  return on;
}

// Replaces cp->RegisterNativeShaderSwap(...) for the shaders that read a multiview target. `flags` as before (kOwnEpilogue ...).
template <class Cp>
inline bool Register(Cp* cp, uint64_t hash, const uint32_t* words, size_t count, const uint8_t* floats, size_t float_count,
                     const char* name, uint32_t flags) {
  if (!On()) return cp->RegisterNativeShaderSwap(hash, words, count, floats, float_count, name, flags);
  for (const Variant& v : kVariants) {
    if (v.plain != words) continue;
    const bool ok = cp->RegisterNativeShaderSwap(hash, words, count, floats, float_count, name, flags | kLayerTrue);
    if (!ok) {
      REXLOG_WARN("[wwe2k14][decouple] '{}' ({:016X}): the replacement did not register, so no layer-true variant either", name ? name : "", hash);
      return false;
    }
    if (!cp->RegisterNativeShaderSwap(hash, v.mv, v.words, nullptr, 0, nullptr, kVariantFlag)) {
      REXLOG_WARN("[wwe2k14][decouple] '{}' ({:016X}): the layer-true variant did not register; the SDK will refuse the swap while gpu_d3d_decouple is on", name ? name : "", hash);
    }
    return true;
  }
  return cp->RegisterNativeShaderSwap(hash, words, count, floats, float_count, name, flags);  // (reads no multiview target)
}

}  // namespace wwe2k14::pixel_multiview
