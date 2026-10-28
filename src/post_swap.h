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
#include "post/spirv/match_combine_enhanced.h"
#include "post/spirv/match_combine_original.h"
#include "post/spirv/match_combine_probe.h"

REXCVAR_DEFINE_BOOL(wwe2k14_post_enhanced, false, "wwe2k14",
                    "Session 72 (with gpu_native_shader_swap, on in NATIVE's list; read at start): our Enhanced post "
                    "effects (the background depth of field as a lens-like disc blur; the launcher's POST EFFECTS: "
                    "ENHANCED); false = our cheaper copy of the game's own pass, step for step (Jarrod, 2026-10-28: the "
                    "default)");
REXCVAR_DEFINE_INT32(wwe2k14_post_swap_probe, 0, "wwe2k14",
                     "Session 72 (diagnostic, needs gpu_native_shader_swap; read at start): 1 = in place of the match and "
                     "entrance combines, probes that paint the passes' inputs and constants for a screenshot "
                     "(tools\\post\\read_probe.py)");

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

template <size_t N, size_t M>
inline bool Register(rex::graphics::CommandProcessor* cp, uint64_t hash, const uint32_t (&words)[N],
                     const uint8_t (&floats)[M], const char* name) {
  return cp->RegisterNativeShaderSwap(hash, words, N, floats, M, name);
}

// Once, after the graphics system exists (OnPostSetup).
inline void Install() {
  rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
  if (!cp) {
    REXLOG_WARN("[wwe2k14][post_swap] no command processor yet: no replacement shaders registered");
    return;
  }
  cp->RegisterNativeRestoreShader(kRestoreColorDepth, 1, 0, "colour + depth restore");
  const bool probe = REXCVAR_GET(wwe2k14_post_swap_probe) == 1;
  const bool enhanced = REXCVAR_GET(wwe2k14_post_enhanced);
  int ok = 0;
  if (probe) {
    ok += Register(cp, kMatchCombine, match_combine_probe_spv, kMatchCombineFloats, "match combine: probe");
    ok += Register(cp, kEntranceCombine, entrance_combine_probe_spv, kEntranceCombineFloats, "entrance combine: probe");
  } else if (enhanced) {
    ok += Register(cp, kMatchCombine, match_combine_enhanced_spv, kMatchCombineFloats, "match combine: Enhanced");
    ok += Register(cp, kEntranceCombine, entrance_combine_enhanced_spv, kEntranceCombineFloats,
                   "entrance combine: Enhanced");
  } else {
    ok += Register(cp, kMatchCombine, match_combine_original_spv, kMatchCombineFloats, "match combine: original");
    ok += Register(cp, kEntranceCombine, entrance_combine_original_spv, kEntranceCombineFloats,
                   "entrance combine: original");
  }
  REXLOG_INFO("[wwe2k14][post_swap] replacement shaders registered: {} ({}; gpu_native_shader_swap = {})", ok,
              probe ? "probes" : enhanced ? "Enhanced" : "original", rex::cvar::GetFlagByName("gpu_native_shader_swap"));
}

}  // namespace wwe2k14::post_swap
