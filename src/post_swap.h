#pragma once
// Session 72 (Jarrod: "chasing right or better" for the post effects): our own post-effect shaders in place of the
// game's on NATIVE's draws. The SDK swaps a guest pixel shader's code for ours and keeps its pass, textures and
// constants (thirdparty\rexglue-sdk-src, vulkan/native/shader_swap.h); the switch is gpu_native_shader_swap (default
// off; live: cvar gpu_native_shader_swap 1). The shaders: src\post\*.frag (shared body src\post\combine.glsl),
// compiled by tools\post\build_shaders.ps1. What the game's post passes do: docs\systems\post_effects.md.

#include <cstdint>
#include <iterator>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "half2_hooks.h"  // wwe2k14::half2::Cp()
#include "post/spirv/entrance_combine_enhanced.h"
#include "post/spirv/entrance_combine_original.h"
#include "post/spirv/entrance_combine_probe.h"
#include "post/spirv/entrance_combine_lite.h"
#include "post/spirv/entrance_combine_lite2.h"
#include "post/spirv/match_combine_lite2.h"
#include "post/spirv/match_combine_lite.h"
#include "post/spirv/match_combine_enhanced.h"
#include "post/spirv/match_combine_original.h"
#include "post/spirv/entrance_combine_original_mp.h"
#include "post/spirv/match_combine_original_mp.h"
#include "post/spirv/match_combine_probe.h"
#include "pixel_multiview.h"  // Session 95 (gpu_d3d_decouple phase 2): the layer-true variants

REXCVAR_DECLARE(bool, wwe2k14_post_enhanced);
REXCVAR_DECLARE(bool, wwe2k14_post_lite);
REXCVAR_DECLARE(bool, wwe2k14_post_lite_fxaa);
REXCVAR_DECLARE(bool, wwe2k14_shader_mediump);
REXCVAR_DECLARE(int32_t, wwe2k14_post_swap_probe);

namespace wwe2k14::post_swap {

// The guest pixel shaders replaced, and the float constant registers each uses, in order (our shaders' packed constant
// blocks are written for them; the SDK refuses a swap whose list does not match the translation's).
// The match's final post pass (console FXAA, the depth of field blend, the depth tint).
inline constexpr uint64_t kMatchCombine = 0x1233F68C15FDF948ull;
inline constexpr uint8_t kMatchCombineFloats[] = {20, 21, 27, 29, 30, 250, 251, 252, 253, 254, 255};
// The entrance's: the same plus bloom (c31) and the star glare (c32).
inline constexpr uint64_t kEntranceCombine = 0x4E2CD8031D93DE3Eull;
inline constexpr uint8_t kEntranceCombineFloats[] = {20, 21, 27, 29, 30, 31, 32, 250, 251, 252, 253, 254, 255};
// Session 72 part 4 (gpu_native_truth_skip_restores): the restore that writes back a colour and a depth resolve
// (microcode: colour target 0 = fetch constant 1, depth = fetch constant 0's x). In entrances it runs right after the
// scene's own two resolves into the same two images (run 4560's segment log): NATIVE leaves it out there.
inline constexpr uint64_t kRestoreColorDepth = 0x9A836B68E3F8F0B0ull;

// (Session 87, stage 4: the combines end in xenos_ps.glsl's XeOutputColor, so they are registered with their own epilogue: the
// swap takes every draw of them, and no registered shader keeps the translator.)
inline constexpr uint32_t kOwnEpilogue = 1;

template <size_t N, size_t M>
inline bool Register(rex::graphics::CommandProcessor* cp, uint64_t hash, const uint32_t (&words)[N],
                     const uint8_t (&floats)[M], const char* name) {
  return wwe2k14::pixel_multiview::Register(cp, hash, words, N, floats, M, name, kOwnEpilogue);  // (== RegisterNativeShaderSwap with decouple off)
}

// Once, after the graphics system exists (OnPostSetup).
inline void Install() {
  rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
  if (!cp) {
    if (wwe2k14::GuestGpuOnly()) return;  // lane 241 (3b): no command processor by design, not a late start
    REXLOG_WARN("[wwe2k14][post_swap] no command processor yet: no replacement shaders registered");
    return;
  }
  cp->RegisterNativeRestoreShader(kRestoreColorDepth, 1, 0, "colour + depth restore");
  const bool probe = REXCVAR_GET(wwe2k14_post_swap_probe) == 1;
  const bool enhanced = REXCVAR_GET(wwe2k14_post_enhanced);
  const bool post_lite = REXCVAR_GET(wwe2k14_post_lite) && !probe && !enhanced;
  if (post_lite) {
    // P1: the SDK's drops of post passes the last combine did not use (glare, bloom chain, depth of field). Applied only if left at
    // default, like the other SDK defaults.
    if (rex::cvar::GetFlagSource("gpu_native_post_lite_drop") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("gpu_native_post_lite_drop", "true");
    }
    if (rex::cvar::GetFlagByName("gpu_native_shader_swap") == "true") {
      REXLOG_INFO("[s95][post_lite] combine variant registered: exact early-outs (blur fetch skipped where its weight is 0, tint where 0); unused post passes dropped (gpu_native_post_lite_drop)");
    }
  }
  int ok = 0;
  int mediump_registered = 0;
  if (probe) {
    ok += Register(cp, kMatchCombine, match_combine_probe_spv, kMatchCombineFloats, "match combine: probe");
    ok += Register(cp, kEntranceCombine, entrance_combine_probe_spv, kEntranceCombineFloats, "entrance combine: probe");
  } else if (enhanced) {
    ok += Register(cp, kMatchCombine, match_combine_enhanced_spv, kMatchCombineFloats, "match combine: Enhanced");
    ok += Register(cp, kEntranceCombine, entrance_combine_enhanced_spv, kEntranceCombineFloats,
                   "entrance combine: Enhanced");
  } else if (post_lite) {
    // Session 95 (post lite): the ORIGINAL combines with exact early-outs (E1). Wins over mediump (they are not combined yet).
    if (REXCVAR_GET(wwe2k14_post_lite_fxaa)) {  // (Session 95, r3gpu)
      ok += Register(cp, kMatchCombine, match_combine_lite2_spv, kMatchCombineFloats, "match combine: lite + FXAA early-out");
      ok += Register(cp, kEntranceCombine, entrance_combine_lite2_spv, kEntranceCombineFloats, "entrance combine: lite + FXAA early-out");
      REXLOG_INFO("[s95][post_lite] wwe2k14_post_lite_fxaa on: the combines skip the FXAA's edge taps on flat pixels");
    } else {
      ok += Register(cp, kMatchCombine, match_combine_lite_spv, kMatchCombineFloats, "match combine: lite");
      ok += Register(cp, kEntranceCombine, entrance_combine_lite_spv, kEntranceCombineFloats, "entrance combine: lite");
    }
  } else if (REXCVAR_GET(wwe2k14_shader_mediump)) {
    // Session 93 (idea 8): the same pass with the colour blends in half precision (FXAA and the depth ramps stay highp).
    ok += Register(cp, kMatchCombine, match_combine_original_mp_spv, kMatchCombineFloats, "match combine: original (mediump)");
    ok += Register(cp, kEntranceCombine, entrance_combine_original_mp_spv, kEntranceCombineFloats,
                   "entrance combine: original (mediump)");
    mediump_registered = 2;
  } else {
    ok += Register(cp, kMatchCombine, match_combine_original_spv, kMatchCombineFloats, "match combine: original");
    ok += Register(cp, kEntranceCombine, entrance_combine_original_spv, kEntranceCombineFloats,
                   "entrance combine: original");
  }
  if (REXCVAR_GET(wwe2k14_shader_mediump)) {
    REXLOG_INFO("[wwe2k14][post_swap] mediump variants registered {} (post combines; Enhanced / probe keep full precision)",
                mediump_registered);
  }
  REXLOG_INFO("[wwe2k14][post_swap] replacement shaders registered: {} ({}; gpu_native_shader_swap = {})", ok,
              probe ? "probes" : enhanced ? "Enhanced" : post_lite ? "lite" : "original", rex::cvar::GetFlagByName("gpu_native_shader_swap"));
}

}  // namespace wwe2k14::post_swap
