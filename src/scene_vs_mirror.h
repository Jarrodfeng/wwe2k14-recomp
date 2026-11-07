#pragma once
// Session 95 (vertex input, Lane A of out\s95\android_structural.md): the vertex-input (_vi) variants of ALL 69 of our vertex shaders (src\scene\spirv\*_vi_vert.h, built by
// tools\shaders\build_vertex_input_vs.py, which also rewrites the generated parts of this file): the vertex comes from vertex attributes - the console's own big-endian bytes,
// swapped in the shader - and the guest index is gl_VertexIndex. The attribute layout is rex/graphics/pipeline/shader/vertex_input_layout.h.
// Registered beside the plain shaders by scene_vs_swap::Install() when the SDK switch gpu_native_vertex_input is not 0; the variant takes the plain entry's flags and float list
// (shader_swap RegisterVertexMirror copies the entry). Taken per draw by the SDK only when BuildVertexInputPlan (vulkan/native/owned_front_own.cpp) proves the draw; the plain code stays for every other draw.
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

// GENERATED-INCLUDES-BEGIN
#include "scene/spirv/arena_vs_bg_vert.h"
#include "scene/spirv/arena_vs_bg_vi_vert.h"
#include "scene/spirv/arena_vs_bg_bump_vert.h"
#include "scene/spirv/arena_vs_bg_bump_vi_vert.h"
#include "scene/spirv/arena_vs_bg_lm_vert.h"
#include "scene/spirv/arena_vs_bg_lm_vi_vert.h"
#include "scene/spirv/arena_vs_bg_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_lm_tf_vi_vert.h"
#include "scene/spirv/arena_vs_bg_rim_vert.h"
#include "scene/spirv/arena_vs_bg_rim_vi_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_vi_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_vi_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_lm_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_lm_vi_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_lm_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_lm_vi_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_vi_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_vi_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_tf_vi_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_plain_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_plain_vi_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_spec_lm_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_spec_lm_vi_vert.h"
#include "scene/spirv/arena_vs_bg_spec_vert.h"
#include "scene/spirv/arena_vs_bg_spec_vi_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_vi_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_lm_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_lm_vi_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_vi_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_tf_vi_vert.h"
#include "scene/spirv/arena_vs_bg_spec_tf_vert.h"
#include "scene/spirv/arena_vs_bg_spec_tf_vi_vert.h"
#include "scene/spirv/arena_vs_bg_tf_vert.h"
#include "scene/spirv/arena_vs_bg_tf_vi_vert.h"
#include "scene/spirv/arena_vs_particle_vert.h"
#include "scene/spirv/arena_vs_particle_vi_vert.h"
#include "scene/spirv/arena_vs_prop_vert.h"
#include "scene/spirv/arena_vs_prop_vi_vert.h"
#include "scene/spirv/arena_vs_prop_bump_vert.h"
#include "scene/spirv/arena_vs_prop_bump_vi_vert.h"
#include "scene/spirv/vs_blur_mask_vert.h"
#include "scene/spirv/vs_blur_mask_vi_vert.h"
#include "scene/spirv/vs_blur_taps_a_vert.h"
#include "scene/spirv/vs_blur_taps_a_vi_vert.h"
#include "scene/spirv/vs_blur_taps_b_vert.h"
#include "scene/spirv/vs_blur_taps_b_vi_vert.h"
#include "scene/spirv/vs_ch_anisotropic_vert.h"
#include "scene/spirv/vs_ch_anisotropic_vi_vert.h"
#include "scene/spirv/vs_ch_gear_w12_vert.h"
#include "scene/spirv/vs_ch_gear_w12_vi_vert.h"
#include "scene/spirv/vs_ch_gear_w9_vert.h"
#include "scene/spirv/vs_ch_gear_w9_vi_vert.h"
#include "scene/spirv/vs_ch_reflect_w11_vert.h"
#include "scene/spirv/vs_ch_reflect_w11_vi_vert.h"
#include "scene/spirv/vs_ch_reflect_w12_vert.h"
#include "scene/spirv/vs_ch_reflect_w12_vi_vert.h"
#include "scene/spirv/vs_ch_reflect_w9_vert.h"
#include "scene/spirv/vs_ch_reflect_w9_vi_vert.h"
#include "scene/spirv/vs_ch_reflect_w9p_vert.h"
#include "scene/spirv/vs_ch_reflect_w9p_vi_vert.h"
#include "scene/spirv/vs_ch_skin_a_vert.h"
#include "scene/spirv/vs_ch_skin_a_vi_vert.h"
#include "scene/spirv/vs_ch_skin_fx_vert.h"
#include "scene/spirv/vs_ch_skin_fx_vi_vert.h"
#include "scene/spirv/vs_ch_skin_lean_vert.h"
#include "scene/spirv/vs_ch_skin_lean_vi_vert.h"
#include "scene/spirv/vs_circle_shadow_vert.h"
#include "scene/spirv/vs_circle_shadow_vi_vert.h"
#include "scene/spirv/vs_crowd_bake_vert.h"
#include "scene/spirv/vs_crowd_bake_vi_vert.h"
#include "scene/spirv/vs_crowd_figure_entrance_vert.h"
#include "scene/spirv/vs_crowd_figure_entrance_vi_vert.h"
#include "scene/spirv/vs_crowd_figure_match_vert.h"
#include "scene/spirv/vs_crowd_figure_match_vi_vert.h"
#include "scene/spirv/vs_default_lit_vert.h"
#include "scene/spirv/vs_default_lit_vi_vert.h"
#include "scene/spirv/vs_flash_vert.h"
#include "scene/spirv/vs_flash_vi_vert.h"
#include "scene/spirv/vs_glare_quad_vert.h"
#include "scene/spirv/vs_glare_quad_vi_vert.h"
#include "scene/spirv/vs_lib_flat_vert.h"
#include "scene/spirv/vs_lib_flat_vi_vert.h"
#include "scene/spirv/vs_lib_quad_vert.h"
#include "scene/spirv/vs_lib_quad_vi_vert.h"
#include "scene/spirv/vs_lib_quad3_vert.h"
#include "scene/spirv/vs_lib_quad3_vi_vert.h"
#include "scene/spirv/vs_light_cube_vert.h"
#include "scene/spirv/vs_light_cube_vi_vert.h"
#include "scene/spirv/vs_liquid_copy_vert.h"
#include "scene/spirv/vs_liquid_copy_vi_vert.h"
#include "scene/spirv/vs_liquid_layer_vert.h"
#include "scene/spirv/vs_liquid_layer_vi_vert.h"
#include "scene/spirv/vs_plane_shadow_vert.h"
#include "scene/spirv/vs_plane_shadow_vi_vert.h"
#include "scene/spirv/vs_plane_shadow_skin_vert.h"
#include "scene/spirv/vs_plane_shadow_skin_vi_vert.h"
#include "scene/spirv/vs_prop_bump_flat_vert.h"
#include "scene/spirv/vs_prop_bump_flat_vi_vert.h"
#include "scene/spirv/vs_prop_bump_skin_vert.h"
#include "scene/spirv/vs_prop_bump_skin_vi_vert.h"
#include "scene/spirv/vs_prop_lit_vert.h"
#include "scene/spirv/vs_prop_lit_vi_vert.h"
#include "scene/spirv/vs_prop_lit_lean_vert.h"
#include "scene/spirv/vs_prop_lit_lean_vi_vert.h"
#include "scene/spirv/vs_prop_lm_vert.h"
#include "scene/spirv/vs_prop_lm_vi_vert.h"
#include "scene/spirv/vs_prop_lm_lit_vert.h"
#include "scene/spirv/vs_prop_lm_lit_vi_vert.h"
#include "scene/spirv/vs_prop_shadow_vert.h"
#include "scene/spirv/vs_prop_shadow_vi_vert.h"
#include "scene/spirv/vs_prop_shadow_lm_vert.h"
#include "scene/spirv/vs_prop_shadow_lm_vi_vert.h"
#include "scene/spirv/vs_rect_vert.h"
#include "scene/spirv/vs_rect_vi_vert.h"
#include "scene/spirv/vs_skin_rim_vert.h"
#include "scene/spirv/vs_skin_rim_vi_vert.h"
#include "scene/spirv/vs_skin_rim11_vert.h"
#include "scene/spirv/vs_skin_rim11_vi_vert.h"
#include "scene/spirv/vs_skin_rim_rigid_vert.h"
#include "scene/spirv/vs_skin_rim_rigid_vi_vert.h"
#include "scene/spirv/vs_skin_shadow_vert.h"
#include "scene/spirv/vs_skin_shadow_vi_vert.h"
#include "scene/spirv/vs_skin_shadow11_vert.h"
#include "scene/spirv/vs_skin_shadow11_vi_vert.h"
#include "scene/spirv/vs_skin_shadow_rigid_vert.h"
#include "scene/spirv/vs_skin_shadow_rigid_vi_vert.h"
#include "scene/spirv/vs_skin_shadow_rigid9_vert.h"
#include "scene/spirv/vs_skin_shadow_rigid9_vi_vert.h"
#include "scene/spirv/vs_sprite_vert.h"
#include "scene/spirv/vs_sprite_vi_vert.h"
// GENERATED-INCLUDES-END

namespace wwe2k14::scene_vs_mirror {

struct Variant {
  const uint32_t* plain;  // the plain shader's SPIR-V (the key: scene_vs_swap's rows point at it)
  const uint32_t* vi;
  size_t words;
};
inline const Variant kVariants[] = {
// GENERATED-VARIANTS-BEGIN
    {arena_vs_bg_vert_spv, arena_vs_bg_vi_vert_spv, std::size(arena_vs_bg_vi_vert_spv)},
    {arena_vs_bg_bump_vert_spv, arena_vs_bg_bump_vi_vert_spv, std::size(arena_vs_bg_bump_vi_vert_spv)},
    {arena_vs_bg_lm_vert_spv, arena_vs_bg_lm_vi_vert_spv, std::size(arena_vs_bg_lm_vi_vert_spv)},
    {arena_vs_bg_lm_tf_vert_spv, arena_vs_bg_lm_tf_vi_vert_spv, std::size(arena_vs_bg_lm_tf_vi_vert_spv)},
    {arena_vs_bg_rim_vert_spv, arena_vs_bg_rim_vi_vert_spv, std::size(arena_vs_bg_rim_vi_vert_spv)},
    {arena_vs_bg_scroll_vert_spv, arena_vs_bg_scroll_vi_vert_spv, std::size(arena_vs_bg_scroll_vi_vert_spv)},
    {arena_vs_bg_scroll2_vert_spv, arena_vs_bg_scroll2_vi_vert_spv, std::size(arena_vs_bg_scroll2_vi_vert_spv)},
    {arena_vs_bg_scroll2_lm_vert_spv, arena_vs_bg_scroll2_lm_vi_vert_spv, std::size(arena_vs_bg_scroll2_lm_vi_vert_spv)},
    {arena_vs_bg_scroll_lm_vert_spv, arena_vs_bg_scroll_lm_vi_vert_spv, std::size(arena_vs_bg_scroll_lm_vi_vert_spv)},
    {arena_vs_bg_shadow_vert_spv, arena_vs_bg_shadow_vi_vert_spv, std::size(arena_vs_bg_shadow_vi_vert_spv)},
    {arena_vs_bg_shadow_lm_vert_spv, arena_vs_bg_shadow_lm_vi_vert_spv, std::size(arena_vs_bg_shadow_lm_vi_vert_spv)},
    {arena_vs_bg_shadow_lm_tf_vert_spv, arena_vs_bg_shadow_lm_tf_vi_vert_spv, std::size(arena_vs_bg_shadow_lm_tf_vi_vert_spv)},
    {arena_vs_bg_shadow_plain_vert_spv, arena_vs_bg_shadow_plain_vi_vert_spv, std::size(arena_vs_bg_shadow_plain_vi_vert_spv)},
    {arena_vs_bg_shadow_spec_lm_vert_spv, arena_vs_bg_shadow_spec_lm_vi_vert_spv, std::size(arena_vs_bg_shadow_spec_lm_vi_vert_spv)},
    {arena_vs_bg_spec_vert_spv, arena_vs_bg_spec_vi_vert_spv, std::size(arena_vs_bg_spec_vi_vert_spv)},
    {arena_vs_bg_spec_fade_vert_spv, arena_vs_bg_spec_fade_vi_vert_spv, std::size(arena_vs_bg_spec_fade_vi_vert_spv)},
    {arena_vs_bg_spec_fade_lm_vert_spv, arena_vs_bg_spec_fade_lm_vi_vert_spv, std::size(arena_vs_bg_spec_fade_lm_vi_vert_spv)},
    {arena_vs_bg_spec_lm_vert_spv, arena_vs_bg_spec_lm_vi_vert_spv, std::size(arena_vs_bg_spec_lm_vi_vert_spv)},
    {arena_vs_bg_spec_lm_tf_vert_spv, arena_vs_bg_spec_lm_tf_vi_vert_spv, std::size(arena_vs_bg_spec_lm_tf_vi_vert_spv)},
    {arena_vs_bg_spec_tf_vert_spv, arena_vs_bg_spec_tf_vi_vert_spv, std::size(arena_vs_bg_spec_tf_vi_vert_spv)},
    {arena_vs_bg_tf_vert_spv, arena_vs_bg_tf_vi_vert_spv, std::size(arena_vs_bg_tf_vi_vert_spv)},
    {arena_vs_particle_vert_spv, arena_vs_particle_vi_vert_spv, std::size(arena_vs_particle_vi_vert_spv)},
    {arena_vs_prop_vert_spv, arena_vs_prop_vi_vert_spv, std::size(arena_vs_prop_vi_vert_spv)},
    {arena_vs_prop_bump_vert_spv, arena_vs_prop_bump_vi_vert_spv, std::size(arena_vs_prop_bump_vi_vert_spv)},
    {vs_blur_mask_vert_spv, vs_blur_mask_vi_vert_spv, std::size(vs_blur_mask_vi_vert_spv)},
    {vs_blur_taps_a_vert_spv, vs_blur_taps_a_vi_vert_spv, std::size(vs_blur_taps_a_vi_vert_spv)},
    {vs_blur_taps_b_vert_spv, vs_blur_taps_b_vi_vert_spv, std::size(vs_blur_taps_b_vi_vert_spv)},
    {vs_ch_anisotropic_vert_spv, vs_ch_anisotropic_vi_vert_spv, std::size(vs_ch_anisotropic_vi_vert_spv)},
    {vs_ch_gear_w12_vert_spv, vs_ch_gear_w12_vi_vert_spv, std::size(vs_ch_gear_w12_vi_vert_spv)},
    {vs_ch_gear_w9_vert_spv, vs_ch_gear_w9_vi_vert_spv, std::size(vs_ch_gear_w9_vi_vert_spv)},
    {vs_ch_reflect_w11_vert_spv, vs_ch_reflect_w11_vi_vert_spv, std::size(vs_ch_reflect_w11_vi_vert_spv)},
    {vs_ch_reflect_w12_vert_spv, vs_ch_reflect_w12_vi_vert_spv, std::size(vs_ch_reflect_w12_vi_vert_spv)},
    {vs_ch_reflect_w9_vert_spv, vs_ch_reflect_w9_vi_vert_spv, std::size(vs_ch_reflect_w9_vi_vert_spv)},
    {vs_ch_reflect_w9p_vert_spv, vs_ch_reflect_w9p_vi_vert_spv, std::size(vs_ch_reflect_w9p_vi_vert_spv)},
    {vs_ch_skin_a_vert_spv, vs_ch_skin_a_vi_vert_spv, std::size(vs_ch_skin_a_vi_vert_spv)},
    {vs_ch_skin_fx_vert_spv, vs_ch_skin_fx_vi_vert_spv, std::size(vs_ch_skin_fx_vi_vert_spv)},
    {vs_ch_skin_lean_vert_spv, vs_ch_skin_lean_vi_vert_spv, std::size(vs_ch_skin_lean_vi_vert_spv)},
    {vs_circle_shadow_vert_spv, vs_circle_shadow_vi_vert_spv, std::size(vs_circle_shadow_vi_vert_spv)},
    {vs_crowd_bake_vert_spv, vs_crowd_bake_vi_vert_spv, std::size(vs_crowd_bake_vi_vert_spv)},
    {vs_crowd_figure_entrance_vert_spv, vs_crowd_figure_entrance_vi_vert_spv, std::size(vs_crowd_figure_entrance_vi_vert_spv)},
    {vs_crowd_figure_match_vert_spv, vs_crowd_figure_match_vi_vert_spv, std::size(vs_crowd_figure_match_vi_vert_spv)},
    {vs_default_lit_vert_spv, vs_default_lit_vi_vert_spv, std::size(vs_default_lit_vi_vert_spv)},
    {vs_flash_vert_spv, vs_flash_vi_vert_spv, std::size(vs_flash_vi_vert_spv)},
    {vs_glare_quad_vert_spv, vs_glare_quad_vi_vert_spv, std::size(vs_glare_quad_vi_vert_spv)},
    {vs_lib_flat_vert_spv, vs_lib_flat_vi_vert_spv, std::size(vs_lib_flat_vi_vert_spv)},
    {vs_lib_quad_vert_spv, vs_lib_quad_vi_vert_spv, std::size(vs_lib_quad_vi_vert_spv)},
    {vs_lib_quad3_vert_spv, vs_lib_quad3_vi_vert_spv, std::size(vs_lib_quad3_vi_vert_spv)},
    {vs_light_cube_vert_spv, vs_light_cube_vi_vert_spv, std::size(vs_light_cube_vi_vert_spv)},
    {vs_liquid_copy_vert_spv, vs_liquid_copy_vi_vert_spv, std::size(vs_liquid_copy_vi_vert_spv)},
    {vs_liquid_layer_vert_spv, vs_liquid_layer_vi_vert_spv, std::size(vs_liquid_layer_vi_vert_spv)},
    {vs_plane_shadow_vert_spv, vs_plane_shadow_vi_vert_spv, std::size(vs_plane_shadow_vi_vert_spv)},
    {vs_plane_shadow_skin_vert_spv, vs_plane_shadow_skin_vi_vert_spv, std::size(vs_plane_shadow_skin_vi_vert_spv)},
    {vs_prop_bump_flat_vert_spv, vs_prop_bump_flat_vi_vert_spv, std::size(vs_prop_bump_flat_vi_vert_spv)},
    {vs_prop_bump_skin_vert_spv, vs_prop_bump_skin_vi_vert_spv, std::size(vs_prop_bump_skin_vi_vert_spv)},
    {vs_prop_lit_vert_spv, vs_prop_lit_vi_vert_spv, std::size(vs_prop_lit_vi_vert_spv)},
    {vs_prop_lit_lean_vert_spv, vs_prop_lit_lean_vi_vert_spv, std::size(vs_prop_lit_lean_vi_vert_spv)},
    {vs_prop_lm_vert_spv, vs_prop_lm_vi_vert_spv, std::size(vs_prop_lm_vi_vert_spv)},
    {vs_prop_lm_lit_vert_spv, vs_prop_lm_lit_vi_vert_spv, std::size(vs_prop_lm_lit_vi_vert_spv)},
    {vs_prop_shadow_vert_spv, vs_prop_shadow_vi_vert_spv, std::size(vs_prop_shadow_vi_vert_spv)},
    {vs_prop_shadow_lm_vert_spv, vs_prop_shadow_lm_vi_vert_spv, std::size(vs_prop_shadow_lm_vi_vert_spv)},
    {vs_rect_vert_spv, vs_rect_vi_vert_spv, std::size(vs_rect_vi_vert_spv)},
    {vs_skin_rim_vert_spv, vs_skin_rim_vi_vert_spv, std::size(vs_skin_rim_vi_vert_spv)},
    {vs_skin_rim11_vert_spv, vs_skin_rim11_vi_vert_spv, std::size(vs_skin_rim11_vi_vert_spv)},
    {vs_skin_rim_rigid_vert_spv, vs_skin_rim_rigid_vi_vert_spv, std::size(vs_skin_rim_rigid_vi_vert_spv)},
    {vs_skin_shadow_vert_spv, vs_skin_shadow_vi_vert_spv, std::size(vs_skin_shadow_vi_vert_spv)},
    {vs_skin_shadow11_vert_spv, vs_skin_shadow11_vi_vert_spv, std::size(vs_skin_shadow11_vi_vert_spv)},
    {vs_skin_shadow_rigid_vert_spv, vs_skin_shadow_rigid_vi_vert_spv, std::size(vs_skin_shadow_rigid_vi_vert_spv)},
    {vs_skin_shadow_rigid9_vert_spv, vs_skin_shadow_rigid9_vi_vert_spv, std::size(vs_skin_shadow_rigid9_vi_vert_spv)},
    {vs_sprite_vert_spv, vs_sprite_vi_vert_spv, std::size(vs_sprite_vi_vert_spv)},
// GENERATED-VARIANTS-END
};

// `mats` = scene_vs_swap::kArenaVertexShaders (any row type with hash / spv); after they are registered. SDK flag bit 28 of
// RegisterNativeVertexSwap = "this is the vertex-input variant of the hash" (VulkanCommandProcessor::RegisterNativeVertexSwap).
template <class Cp, class Mat>
inline int RegisterAll(Cp* cp, const Mat* mats, size_t count) {
  if (rex::cvar::GetFlagByName("gpu_native_vertex_input") == "0") return 0;  // (Session 95: 1 or 2 want our _vi modules)
  int ok = 0, missing = 0;
  for (size_t i = 0; i < count; ++i) {
    bool found = false;
    for (const Variant& v : kVariants) {
      if (v.plain == mats[i].spv) {
        ok += cp->RegisterNativeVertexSwap(mats[i].hash, v.vi, v.words, nullptr, 0, nullptr, 0x10000000u, 0, 0) ? 1 : 0;
        found = true;
        break;
      }
    }
    missing += found ? 0 : 1;
  }
  REXLOG_INFO("[mirror] vertex-input variants registered: {} ({} of our shaders have none: they read the shared memory)", ok, missing);
  return ok;
}

}  // namespace wwe2k14::scene_vs_mirror
