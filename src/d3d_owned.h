#pragma once
// Session 87 (Jarrod: "merge ... we should try this behind a toggle for now"): one switch for the whole D3D-layer stack -
// the game's Direct3D calls to our own renderer (docs\native\D3D_LAYER_PLAN.md, the four stages) plus the owned-frame
// chains, our own shaders without the Xbox 360 translator, textures by game object and the crowd drawn by our program.
// It sets the session's final-state line (out\s87_flags_all.txt, checked runs 211-217) without the verify / diagnostic
// switches; each switch only if left at its default, so any of them can still be overridden on the command line.
// Default off: an experimental toggle, not a player default.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <utility>

REXCVAR_DEFINE_BOOL(wwe2k14_d3d_owned, false, "wwe2k14",
                    "Session 87 (experimental): the whole D3D-layer stack in one switch - the game's Direct3D calls go "
                    "to our own renderer (no command ring, no register file, our own shaders, textures by game object, "
                    "the crowd by our program, the owned-frame chains). Sets each of its switches only if left at its "
                    "default (src\\d3d_owned.h). Default off");

namespace wwe2k14::d3d_owned {

// OnPreSetup, after native_frame::Apply(): every switch the final-state check ran with, minus the verify modes.
inline void Apply() {
  if (!REXCVAR_GET(wwe2k14_d3d_owned)) return;
  static constexpr std::pair<const char*, const char*> kFlags[] = {
      // The owned frame and its Session 87 steps.
      {"gpu_native_owned_frame", "true"},
      {"gpu_native_owned_frame_hold_msaa_clears", "true"},
      {"gpu_native_owned_frame_continue_copy", "true"},
      {"gpu_native_owned_frame_restore_draw", "true"},
      {"gpu_native_owned_frame_carry_passes", "true"},
      {"gpu_native_owned_frame_tiles_memo", "true"},
      {"gpu_native_owned_frame_dead_relearn", "true"},
      {"gpu_native_owned_frame_strip_batch", "true"},
      {"gpu_native_owned_frame_defined_start_16", "true"},
      {"gpu_native_owned_frame_chains", "true"},
      {"wwe2k14_scene_crowd_cards", "true"},
      // Stage 1: the draw state leaves the command stream.
      {"wwe2k14_d3d_layer", "true"},
      {"wwe2k14_d3d_copy_mip", "true"},
      {"wwe2k14_d3d_copy_depthonly", "true"},
      // Stage 2: no ring.
      {"gpu_d3d_noring", "1"},
      {"wwe2k14_d3d_noring", "3"},
      // Stage 3: our renderer fed from the records.
      {"gpu_native_lean_multi_base", "true"},
      {"vulkan_repeat_owned_without_layout", "true"},
      {"gpu_native_lean_state_cache", "true"},
      {"gpu_d3d_direct", "true"},
      {"gpu_d3d_own_input", "true"},
      {"gpu_d3d_own_full", "true"},
      {"gpu_d3d_own_crowd", "true"},
      {"gpu_d3d_own_copy", "true"},
      // Stage 4: our own shaders (the translator skipped), the crowd by our program, textures by game object.
      {"wwe2k14_vs_swap", "true"},
      {"gpu_native_swap_skip_translation", "true"},
      {"gpu_native_swap_skip_translation_pixel", "true"},
      {"gpu_native_own_crowd_all", "true"},
      {"wwe2k14_d3d_textures", "2"},
  };
  int set = 0;
  for (const auto& [name, value] : kFlags) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName(name, value);
      ++set;
    }
  }
  REXLOG_INFO("[wwe2k14][d3d_owned] the D3D-layer stack on: {} of {} switches set (the rest given on the command line)",
              set, std::size(kFlags));
}

}  // namespace wwe2k14::d3d_owned
