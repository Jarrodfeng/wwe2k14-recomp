// Session 37: the light renderer, for integrated graphics and other weak GPUs (a playtester's Intel UHD 620 laptop:
// 25-30 fps in the menus, 5-10 in entrances and matches with the accurate path).
//
// The accurate path (render_target_path_vulkan=fsi, the default since Session 3) does the Xbox 360's EDRAM work
// (blending, depth, formats) in every pixel shader under fragment shader interlock: long shaders, no early depth
// rejection. The host render target path (fbo) uses the GPU's own render targets and blending. It had two faults here:
//   1. Whole black frames (Sessions 3 / 22 / 37: 7 of 8 match shots, 5 of 15 replay shots). Cause (Session 37, runs
//      2605-2607, SDK gpu_rt_update_log / resolve_stats): the frame's last draw before the final copy is a depth-only
//      rectangle on a 4x MSAA surface with clipping off and an 8192 scissor; without the CPU vertex estimate its
//      render target "uses" the surface's full height, takes ownership of all 2048 EDRAM tiles - the frame's colour
//      included - and the final copy then reads that depth target (zeros). ReXGlue ships
//      execute_unclipped_draw_vs_on_cpu = false; Xenia's own default is true, and its help text describes exactly
//      this ("up to the entire EDRAM may be considered used ... corruption of other render targets").
//   2. Depth and blending differences (missing star glints, crowd / depth differences in entrances; Session 3's
//      "Punk-style" depth errors): the 360's 24-bit float depth stored as 32-bit float. Xenia's
//      depth_float24_convert_in_pixel_shader converts it exactly per pixel. Session 37 replay sheet (s22_fbo_shots,
//      runs 2610 / 2611 / 2612): pixels off by > 32 against the accurate path 0.9-11.2% without it, <= 0.001% with it.
// --wwe2k14_light_renderer=true sets those three (each only if left at default). Default off: the accurate path stays.
//
// --wwe2k14_fbo_fallback_fix (default ON since 2026-10-20, Jarrod: "untested but it sounds like everything is properly
// broken"): a card without interlock (AMD's Windows driver before Adrenalin 24.12.1, older AMD graphics; Session 54)
// gets the host render target path from the SDK by itself,
// with both faults. This applies the two fixes there too. Cards with interlock are not affected.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu_pick.h"

REXCVAR_DEFINE_BOOL(wwe2k14_light_renderer, false, "wwe2k14",
                    "Session 37: the light renderer for integrated graphics / weak GPUs: the host render target path "
                    "(render_target_path_vulkan=fbo) with its black-frame and depth fixes. Default off (accurate path)");
REXCVAR_DEFINE_BOOL(wwe2k14_fbo_fallback_fix, true, "wwe2k14",
                    "Session 37: on a graphics card without fragment shader interlock (the SDK then uses the host "
                    "render target path itself, e.g. older AMD drivers), apply the light renderer's fixes. Default on "
                    "(Jarrod, 2026-10-20); no effect on cards with interlock");

// Session 54 (part B: NATIVE without interlock, tested on this PC's 4090): the game treats the chosen card as one without
// fragment shader interlock - the SDK's host render target path (fbo) and the fixes such a card gets
// (--wwe2k14_fbo_fallback_fix), as an AMD card on a driver before Adrenalin 24.12.1 gets them. Test only.
REXCVAR_DEFINE_BOOL(wwe2k14_test_no_interlock, false, "wwe2k14",
                    "Session 54 (test): treat the graphics card as one without fragment shader interlock (the host "
                    "render target path and its fixes, as a card without it gets); for testing that path on a card "
                    "that has it. Default off");

namespace wwe2k14::light {

inline void SetIfDefault(const char* name, const char* value) {
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
}

inline void ApplyFixes() {
  SetIfDefault("execute_unclipped_draw_vs_on_cpu", "true");
  SetIfDefault("depth_float24_convert_in_pixel_shader", "true");
}

// OnPreSetup, before the app's own render path default (fsi).
inline void Configure() {
  if (!REXCVAR_GET(wwe2k14_light_renderer)) return;
  SetIfDefault("render_target_path_vulkan", "fbo");
  ApplyFixes();
  REXLOG_INFO("[wwe2k14][light] light renderer: render_target_path_vulkan={}, execute_unclipped_draw_vs_on_cpu={}, "
              "depth_float24_convert_in_pixel_shader={}",
              rex::cvar::GetFlagByName("render_target_path_vulkan"),
              rex::cvar::GetFlagByName("execute_unclipped_draw_vs_on_cpu"),
              rex::cvar::GetFlagByName("depth_float24_convert_in_pixel_shader"));
}

// OnPreSetup, after gpu_pick::Choose (the chosen device is known).
inline void ConfigureFallback() {
  if (!gpu_pick::g_known) return;
#if defined(__ANDROID__)
  // Session 73: phones stay on the host render target path. Session 63 assumed "Adreno has no interlock" (true of
  // Jarrod's 830 on its driver 512.800), but newer Qualcomm drivers offer it (Adreno 710 / 725 / 830 on 512.863+,
  // vulkan.gpuinfo.org), and there the game took the accurate path (EDRAM work in every pixel shader under interlock),
  // never run on a phone and the worst fit for a tiling GPU. Unless the path is set on the command line.
  if (gpu_pick::g_chosen.interlock &&
      rex::cvar::GetFlagSource("render_target_path_vulkan") != rex::cvar::Source::kCommandLine) {
    REXLOG_INFO("[wwe2k14][light] phone GPU with interlock ({}): host render target path, as on every phone",
                gpu_pick::g_chosen.name);
    gpu_pick::g_chosen.interlock = false;
    rex::cvar::SetFlagByName("render_target_path_vulkan", "fbo");
  }
#endif
  if (REXCVAR_GET(wwe2k14_test_no_interlock)) {  // Session 54 (test)
    gpu_pick::g_chosen.interlock = false;
    rex::cvar::SetFlagByName("render_target_path_vulkan", "fbo");  // (the app set fsi as its default before this)
  }
  const auto& d = gpu_pick::g_chosen;
  const bool fbo = rex::cvar::GetFlagByName("render_target_path_vulkan") != "fsi" || !d.interlock;
  REXLOG_INFO("[wwe2k14][light] graphics card: {} ({}); accurate path (interlock) {}; render path {}", d.name,
              d.type_text(), d.interlock ? "supported" : "NOT supported",
              fbo ? "host render targets (fbo)" : "accurate (fsi)");
  if (d.interlock || REXCVAR_GET(wwe2k14_light_renderer)) return;
  // Session 54: NATIVE there draws its truth frames on the card's own render targets (SDK gpu_native_truth_host_rt).
  if (rex::cvar::GetFlagByName("gpu_native") == "2")
    REXLOG_INFO("[wwe2k14][light] no interlock: the NATIVE renderer runs on the host render target path (truth frames "
                "only: gpu_native_truth_host_rt {})",
                rex::cvar::GetFlagByName("gpu_native_truth_host_rt"));
  if (REXCVAR_GET(wwe2k14_fbo_fallback_fix)) {
    ApplyFixes();
    REXLOG_INFO("[wwe2k14][light] no interlock: the SDK falls back to the host render target path; its fixes applied "
                "(--wwe2k14_fbo_fallback_fix)");
  } else {
    REXLOG_WARN("[wwe2k14][light] no interlock on this card: the SDK falls back to the host render target path "
                "WITHOUT its fixes (black frames likely); --wwe2k14_light_renderer=true or "
                "--wwe2k14_fbo_fallback_fix=true fix that");
  }
}

}  // namespace wwe2k14::light
