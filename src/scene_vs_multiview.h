#pragma once
// Session 94 (idea 1 phase 1B, gpu_d3d_decouple bit 3): the multiview variants of our vertex shaders (src\scene\spirv\*_mv_vert.h, built by tools\shaders\build_multiview_vs.py: every
// float constant read at register + gl_ViewIndex * XE_VSTRIDE; the SDK patches the stride, specialization constant 1, to 256). Registered beside the plain shaders by
// scene_vs_swap::Install() when the SDK switch gpu_d3d_decouple has bit 3 (value 8) set; with it the SDK runs ONLY these for a taken vertex shader (no variant = the game's translated shader stays).
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "scene/spirv/arena_vs_bg_bump_vert.h"
#include "scene/spirv/arena_vs_bg_bump_mv_vert.h"
#include "scene/spirv/arena_vs_bg_lm_vert.h"
#include "scene/spirv/arena_vs_bg_lm_mv_vert.h"
#include "scene/spirv/arena_vs_bg_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_lm_tf_mv_vert.h"
#include "scene/spirv/arena_vs_bg_vert.h"
#include "scene/spirv/arena_vs_bg_mv_vert.h"
#include "scene/spirv/arena_vs_bg_rim_vert.h"
#include "scene/spirv/arena_vs_bg_rim_mv_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_lm_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_lm_mv_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_vert.h"
#include "scene/spirv/arena_vs_bg_scroll2_mv_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_lm_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_lm_mv_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_vert.h"
#include "scene/spirv/arena_vs_bg_scroll_mv_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_mv_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_lm_tf_mv_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_mv_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_plain_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_plain_mv_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_spec_lm_vert.h"
#include "scene/spirv/arena_vs_bg_shadow_spec_lm_mv_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_lm_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_lm_mv_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_vert.h"
#include "scene/spirv/arena_vs_bg_spec_fade_mv_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_mv_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_tf_vert.h"
#include "scene/spirv/arena_vs_bg_spec_lm_tf_mv_vert.h"
#include "scene/spirv/arena_vs_bg_spec_vert.h"
#include "scene/spirv/arena_vs_bg_spec_mv_vert.h"
#include "scene/spirv/arena_vs_bg_spec_tf_vert.h"
#include "scene/spirv/arena_vs_bg_spec_tf_mv_vert.h"
#include "scene/spirv/arena_vs_bg_tf_vert.h"
#include "scene/spirv/arena_vs_bg_tf_mv_vert.h"
#include "scene/spirv/arena_vs_particle_vert.h"
#include "scene/spirv/arena_vs_particle_mv_vert.h"
#include "scene/spirv/arena_vs_prop_bump_vert.h"
#include "scene/spirv/arena_vs_prop_bump_mv_vert.h"
#include "scene/spirv/arena_vs_prop_vert.h"
#include "scene/spirv/arena_vs_prop_mv_vert.h"
#include "scene/spirv/vs_blur_mask_vert.h"
#include "scene/spirv/vs_blur_mask_mv_vert.h"
#include "scene/spirv/vs_blur_taps_a_vert.h"
#include "scene/spirv/vs_blur_taps_a_mv_vert.h"
#include "scene/spirv/vs_blur_taps_b_vert.h"
#include "scene/spirv/vs_blur_taps_b_mv_vert.h"
#include "scene/spirv/vs_ch_anisotropic_vert.h"
#include "scene/spirv/vs_ch_anisotropic_mv_vert.h"
#include "scene/spirv/vs_ch_gear_w12_vert.h"
#include "scene/spirv/vs_ch_gear_w12_mv_vert.h"
#include "scene/spirv/vs_ch_gear_w9_vert.h"
#include "scene/spirv/vs_ch_gear_w9_mv_vert.h"
#include "scene/spirv/vs_ch_reflect_w11_vert.h"
#include "scene/spirv/vs_ch_reflect_w11_mv_vert.h"
#include "scene/spirv/vs_ch_reflect_w12_vert.h"
#include "scene/spirv/vs_ch_reflect_w12_mv_vert.h"
#include "scene/spirv/vs_ch_reflect_w9_vert.h"
#include "scene/spirv/vs_ch_reflect_w9_mv_vert.h"
#include "scene/spirv/vs_ch_reflect_w9p_vert.h"
#include "scene/spirv/vs_ch_reflect_w9p_mv_vert.h"
#include "scene/spirv/vs_ch_skin_a_vert.h"
#include "scene/spirv/vs_ch_skin_a_mv_vert.h"
#include "scene/spirv/vs_ch_skin_fx_vert.h"
#include "scene/spirv/vs_ch_skin_fx_mv_vert.h"
#include "scene/spirv/vs_ch_skin_lean_vert.h"
#include "scene/spirv/vs_ch_skin_lean_mv_vert.h"
#include "scene/spirv/vs_circle_shadow_vert.h"
#include "scene/spirv/vs_circle_shadow_mv_vert.h"
#include "scene/spirv/vs_crowd_bake_vert.h"
#include "scene/spirv/vs_crowd_bake_mv_vert.h"
#include "scene/spirv/vs_crowd_figure_entrance_vert.h"
#include "scene/spirv/vs_crowd_figure_entrance_mv_vert.h"
#include "scene/spirv/vs_crowd_figure_match_vert.h"
#include "scene/spirv/vs_crowd_figure_match_mv_vert.h"
#include "scene/spirv/vs_default_lit_vert.h"
#include "scene/spirv/vs_default_lit_mv_vert.h"
#include "scene/spirv/vs_flash_vert.h"
#include "scene/spirv/vs_flash_mv_vert.h"
#include "scene/spirv/vs_glare_quad_vert.h"
#include "scene/spirv/vs_glare_quad_mv_vert.h"
#include "scene/spirv/vs_lib_flat_vert.h"
#include "scene/spirv/vs_lib_flat_mv_vert.h"
#include "scene/spirv/vs_lib_quad3_vert.h"
#include "scene/spirv/vs_lib_quad3_mv_vert.h"
#include "scene/spirv/vs_lib_quad_vert.h"
#include "scene/spirv/vs_lib_quad_mv_vert.h"
#include "scene/spirv/vs_light_cube_vert.h"
#include "scene/spirv/vs_light_cube_mv_vert.h"
#include "scene/spirv/vs_liquid_copy_vert.h"
#include "scene/spirv/vs_liquid_copy_mv_vert.h"
#include "scene/spirv/vs_liquid_layer_vert.h"
#include "scene/spirv/vs_liquid_layer_mv_vert.h"
#include "scene/spirv/vs_plane_shadow_vert.h"
#include "scene/spirv/vs_plane_shadow_mv_vert.h"
#include "scene/spirv/vs_plane_shadow_skin_vert.h"
#include "scene/spirv/vs_plane_shadow_skin_mv_vert.h"
#include "scene/spirv/vs_prop_bump_flat_vert.h"
#include "scene/spirv/vs_prop_bump_flat_mv_vert.h"
#include "scene/spirv/vs_prop_bump_skin_vert.h"
#include "scene/spirv/vs_prop_bump_skin_mv_vert.h"
#include "scene/spirv/vs_prop_lit_lean_vert.h"
#include "scene/spirv/vs_prop_lit_lean_mv_vert.h"
#include "scene/spirv/vs_prop_lit_vert.h"
#include "scene/spirv/vs_prop_lit_mv_vert.h"
#include "scene/spirv/vs_prop_lm_lit_vert.h"
#include "scene/spirv/vs_prop_lm_lit_mv_vert.h"
#include "scene/spirv/vs_prop_lm_vert.h"
#include "scene/spirv/vs_prop_lm_mv_vert.h"
#include "scene/spirv/vs_prop_shadow_lm_vert.h"
#include "scene/spirv/vs_prop_shadow_lm_mv_vert.h"
#include "scene/spirv/vs_prop_shadow_vert.h"
#include "scene/spirv/vs_prop_shadow_mv_vert.h"
#include "scene/spirv/vs_rect_vert.h"
#include "scene/spirv/vs_rect_mv_vert.h"
#include "scene/spirv/vs_skin_rim11_vert.h"
#include "scene/spirv/vs_skin_rim11_mv_vert.h"
#include "scene/spirv/vs_skin_rim_vert.h"
#include "scene/spirv/vs_skin_rim_mv_vert.h"
#include "scene/spirv/vs_skin_rim_rigid_vert.h"
#include "scene/spirv/vs_skin_rim_rigid_mv_vert.h"
#include "scene/spirv/vs_skin_shadow11_vert.h"
#include "scene/spirv/vs_skin_shadow11_mv_vert.h"
#include "scene/spirv/vs_skin_shadow_vert.h"
#include "scene/spirv/vs_skin_shadow_mv_vert.h"
#include "scene/spirv/vs_skin_shadow_rigid9_vert.h"
#include "scene/spirv/vs_skin_shadow_rigid9_mv_vert.h"
#include "scene/spirv/vs_skin_shadow_rigid_vert.h"
#include "scene/spirv/vs_skin_shadow_rigid_mv_vert.h"
#include "scene/spirv/vs_sprite_vert.h"
#include "scene/spirv/vs_sprite_mv_vert.h"

namespace wwe2k14::scene_vs_multiview {

struct Variant {
  const uint32_t* plain;  // the plain shader's SPIR-V (the key: scene_vs_swap's rows point at it)
  const uint32_t* mv;
  size_t words;
};
inline const Variant kVariants[] = {
    {arena_vs_bg_bump_vert_spv, arena_vs_bg_bump_mv_vert_spv, std::size(arena_vs_bg_bump_mv_vert_spv)},
    {arena_vs_bg_lm_vert_spv, arena_vs_bg_lm_mv_vert_spv, std::size(arena_vs_bg_lm_mv_vert_spv)},
    {arena_vs_bg_lm_tf_vert_spv, arena_vs_bg_lm_tf_mv_vert_spv, std::size(arena_vs_bg_lm_tf_mv_vert_spv)},
    {arena_vs_bg_vert_spv, arena_vs_bg_mv_vert_spv, std::size(arena_vs_bg_mv_vert_spv)},
    {arena_vs_bg_rim_vert_spv, arena_vs_bg_rim_mv_vert_spv, std::size(arena_vs_bg_rim_mv_vert_spv)},
    {arena_vs_bg_scroll2_lm_vert_spv, arena_vs_bg_scroll2_lm_mv_vert_spv, std::size(arena_vs_bg_scroll2_lm_mv_vert_spv)},
    {arena_vs_bg_scroll2_vert_spv, arena_vs_bg_scroll2_mv_vert_spv, std::size(arena_vs_bg_scroll2_mv_vert_spv)},
    {arena_vs_bg_scroll_lm_vert_spv, arena_vs_bg_scroll_lm_mv_vert_spv, std::size(arena_vs_bg_scroll_lm_mv_vert_spv)},
    {arena_vs_bg_scroll_vert_spv, arena_vs_bg_scroll_mv_vert_spv, std::size(arena_vs_bg_scroll_mv_vert_spv)},
    {arena_vs_bg_shadow_lm_vert_spv, arena_vs_bg_shadow_lm_mv_vert_spv, std::size(arena_vs_bg_shadow_lm_mv_vert_spv)},
    {arena_vs_bg_shadow_lm_tf_vert_spv, arena_vs_bg_shadow_lm_tf_mv_vert_spv, std::size(arena_vs_bg_shadow_lm_tf_mv_vert_spv)},
    {arena_vs_bg_shadow_vert_spv, arena_vs_bg_shadow_mv_vert_spv, std::size(arena_vs_bg_shadow_mv_vert_spv)},
    {arena_vs_bg_shadow_plain_vert_spv, arena_vs_bg_shadow_plain_mv_vert_spv, std::size(arena_vs_bg_shadow_plain_mv_vert_spv)},
    {arena_vs_bg_shadow_spec_lm_vert_spv, arena_vs_bg_shadow_spec_lm_mv_vert_spv, std::size(arena_vs_bg_shadow_spec_lm_mv_vert_spv)},
    {arena_vs_bg_spec_fade_lm_vert_spv, arena_vs_bg_spec_fade_lm_mv_vert_spv, std::size(arena_vs_bg_spec_fade_lm_mv_vert_spv)},
    {arena_vs_bg_spec_fade_vert_spv, arena_vs_bg_spec_fade_mv_vert_spv, std::size(arena_vs_bg_spec_fade_mv_vert_spv)},
    {arena_vs_bg_spec_lm_vert_spv, arena_vs_bg_spec_lm_mv_vert_spv, std::size(arena_vs_bg_spec_lm_mv_vert_spv)},
    {arena_vs_bg_spec_lm_tf_vert_spv, arena_vs_bg_spec_lm_tf_mv_vert_spv, std::size(arena_vs_bg_spec_lm_tf_mv_vert_spv)},
    {arena_vs_bg_spec_vert_spv, arena_vs_bg_spec_mv_vert_spv, std::size(arena_vs_bg_spec_mv_vert_spv)},
    {arena_vs_bg_spec_tf_vert_spv, arena_vs_bg_spec_tf_mv_vert_spv, std::size(arena_vs_bg_spec_tf_mv_vert_spv)},
    {arena_vs_bg_tf_vert_spv, arena_vs_bg_tf_mv_vert_spv, std::size(arena_vs_bg_tf_mv_vert_spv)},
    {arena_vs_particle_vert_spv, arena_vs_particle_mv_vert_spv, std::size(arena_vs_particle_mv_vert_spv)},
    {arena_vs_prop_bump_vert_spv, arena_vs_prop_bump_mv_vert_spv, std::size(arena_vs_prop_bump_mv_vert_spv)},
    {arena_vs_prop_vert_spv, arena_vs_prop_mv_vert_spv, std::size(arena_vs_prop_mv_vert_spv)},
    {vs_blur_mask_vert_spv, vs_blur_mask_mv_vert_spv, std::size(vs_blur_mask_mv_vert_spv)},
    {vs_blur_taps_a_vert_spv, vs_blur_taps_a_mv_vert_spv, std::size(vs_blur_taps_a_mv_vert_spv)},
    {vs_blur_taps_b_vert_spv, vs_blur_taps_b_mv_vert_spv, std::size(vs_blur_taps_b_mv_vert_spv)},
    {vs_ch_anisotropic_vert_spv, vs_ch_anisotropic_mv_vert_spv, std::size(vs_ch_anisotropic_mv_vert_spv)},
    {vs_ch_gear_w12_vert_spv, vs_ch_gear_w12_mv_vert_spv, std::size(vs_ch_gear_w12_mv_vert_spv)},
    {vs_ch_gear_w9_vert_spv, vs_ch_gear_w9_mv_vert_spv, std::size(vs_ch_gear_w9_mv_vert_spv)},
    {vs_ch_reflect_w11_vert_spv, vs_ch_reflect_w11_mv_vert_spv, std::size(vs_ch_reflect_w11_mv_vert_spv)},
    {vs_ch_reflect_w12_vert_spv, vs_ch_reflect_w12_mv_vert_spv, std::size(vs_ch_reflect_w12_mv_vert_spv)},
    {vs_ch_reflect_w9_vert_spv, vs_ch_reflect_w9_mv_vert_spv, std::size(vs_ch_reflect_w9_mv_vert_spv)},
    {vs_ch_reflect_w9p_vert_spv, vs_ch_reflect_w9p_mv_vert_spv, std::size(vs_ch_reflect_w9p_mv_vert_spv)},
    {vs_ch_skin_a_vert_spv, vs_ch_skin_a_mv_vert_spv, std::size(vs_ch_skin_a_mv_vert_spv)},
    {vs_ch_skin_fx_vert_spv, vs_ch_skin_fx_mv_vert_spv, std::size(vs_ch_skin_fx_mv_vert_spv)},
    {vs_ch_skin_lean_vert_spv, vs_ch_skin_lean_mv_vert_spv, std::size(vs_ch_skin_lean_mv_vert_spv)},
    {vs_circle_shadow_vert_spv, vs_circle_shadow_mv_vert_spv, std::size(vs_circle_shadow_mv_vert_spv)},
    {vs_crowd_bake_vert_spv, vs_crowd_bake_mv_vert_spv, std::size(vs_crowd_bake_mv_vert_spv)},
    {vs_crowd_figure_entrance_vert_spv, vs_crowd_figure_entrance_mv_vert_spv, std::size(vs_crowd_figure_entrance_mv_vert_spv)},
    {vs_crowd_figure_match_vert_spv, vs_crowd_figure_match_mv_vert_spv, std::size(vs_crowd_figure_match_mv_vert_spv)},
    {vs_default_lit_vert_spv, vs_default_lit_mv_vert_spv, std::size(vs_default_lit_mv_vert_spv)},
    {vs_flash_vert_spv, vs_flash_mv_vert_spv, std::size(vs_flash_mv_vert_spv)},
    {vs_glare_quad_vert_spv, vs_glare_quad_mv_vert_spv, std::size(vs_glare_quad_mv_vert_spv)},
    {vs_lib_flat_vert_spv, vs_lib_flat_mv_vert_spv, std::size(vs_lib_flat_mv_vert_spv)},
    {vs_lib_quad3_vert_spv, vs_lib_quad3_mv_vert_spv, std::size(vs_lib_quad3_mv_vert_spv)},
    {vs_lib_quad_vert_spv, vs_lib_quad_mv_vert_spv, std::size(vs_lib_quad_mv_vert_spv)},
    {vs_light_cube_vert_spv, vs_light_cube_mv_vert_spv, std::size(vs_light_cube_mv_vert_spv)},
    {vs_liquid_copy_vert_spv, vs_liquid_copy_mv_vert_spv, std::size(vs_liquid_copy_mv_vert_spv)},
    {vs_liquid_layer_vert_spv, vs_liquid_layer_mv_vert_spv, std::size(vs_liquid_layer_mv_vert_spv)},
    {vs_plane_shadow_vert_spv, vs_plane_shadow_mv_vert_spv, std::size(vs_plane_shadow_mv_vert_spv)},
    {vs_plane_shadow_skin_vert_spv, vs_plane_shadow_skin_mv_vert_spv, std::size(vs_plane_shadow_skin_mv_vert_spv)},
    {vs_prop_bump_flat_vert_spv, vs_prop_bump_flat_mv_vert_spv, std::size(vs_prop_bump_flat_mv_vert_spv)},
    {vs_prop_bump_skin_vert_spv, vs_prop_bump_skin_mv_vert_spv, std::size(vs_prop_bump_skin_mv_vert_spv)},
    {vs_prop_lit_lean_vert_spv, vs_prop_lit_lean_mv_vert_spv, std::size(vs_prop_lit_lean_mv_vert_spv)},
    {vs_prop_lit_vert_spv, vs_prop_lit_mv_vert_spv, std::size(vs_prop_lit_mv_vert_spv)},
    {vs_prop_lm_lit_vert_spv, vs_prop_lm_lit_mv_vert_spv, std::size(vs_prop_lm_lit_mv_vert_spv)},
    {vs_prop_lm_vert_spv, vs_prop_lm_mv_vert_spv, std::size(vs_prop_lm_mv_vert_spv)},
    {vs_prop_shadow_lm_vert_spv, vs_prop_shadow_lm_mv_vert_spv, std::size(vs_prop_shadow_lm_mv_vert_spv)},
    {vs_prop_shadow_vert_spv, vs_prop_shadow_mv_vert_spv, std::size(vs_prop_shadow_mv_vert_spv)},
    {vs_rect_vert_spv, vs_rect_mv_vert_spv, std::size(vs_rect_mv_vert_spv)},
    {vs_skin_rim11_vert_spv, vs_skin_rim11_mv_vert_spv, std::size(vs_skin_rim11_mv_vert_spv)},
    {vs_skin_rim_vert_spv, vs_skin_rim_mv_vert_spv, std::size(vs_skin_rim_mv_vert_spv)},
    {vs_skin_rim_rigid_vert_spv, vs_skin_rim_rigid_mv_vert_spv, std::size(vs_skin_rim_rigid_mv_vert_spv)},
    {vs_skin_shadow11_vert_spv, vs_skin_shadow11_mv_vert_spv, std::size(vs_skin_shadow11_mv_vert_spv)},
    {vs_skin_shadow_vert_spv, vs_skin_shadow_mv_vert_spv, std::size(vs_skin_shadow_mv_vert_spv)},
    {vs_skin_shadow_rigid9_vert_spv, vs_skin_shadow_rigid9_mv_vert_spv, std::size(vs_skin_shadow_rigid9_mv_vert_spv)},
    {vs_skin_shadow_rigid_vert_spv, vs_skin_shadow_rigid_mv_vert_spv, std::size(vs_skin_shadow_rigid_mv_vert_spv)},
    {vs_sprite_vert_spv, vs_sprite_mv_vert_spv, std::size(vs_sprite_mv_vert_spv)},
};

// `mats` = scene_vs_swap::kArenaVertexShaders (any row type with hash / spv); after they are registered. SDK flag bit 29 of
// RegisterNativeVertexSwap = "this is the multiview variant of the hash" (VulkanCommandProcessor::RegisterNativeVertexSwap).
template <class Cp, class Mat>
inline int RegisterAll(Cp* cp, const Mat* mats, size_t count) {
  int mode = 0;
  try {
    mode = std::stoi(rex::cvar::GetFlagByName("gpu_d3d_decouple"));
  } catch (...) {
  }
  if (!(mode & 8)) return 0;
  int ok = 0, missing = 0;
  for (size_t i = 0; i < count; ++i) {
    bool found = false;
    for (const Variant& v : kVariants) {
      if (v.plain == mats[i].spv) {
        ok += cp->RegisterNativeVertexSwap(mats[i].hash, v.mv, v.words, nullptr, 0, nullptr, 0x20000000u, 0, 0) ? 1 : 0;
        found = true;
        break;
      }
    }
    missing += found ? 0 : 1;
  }
  REXLOG_INFO("[native] decouple: {} multiview vertex shader variants registered ({} of our shaders have none: the game's translated shader draws them)", ok, missing);
  return ok;
}

}  // namespace wwe2k14::scene_vs_multiview
