// The scene skips, game side (Session 103, lane H; docs\renderer\SCENE_SKIPS.md). COMPILED, NOT RUN (0 game runs in this lane).
//
// The switch --wwe2k14_rdr_scene (FRAME_CONTRACT.md section 2) is DEFINED in renderer_adapter/live.cpp; this file hands its storage to scene_frame (the per-frame snapshot
// taken in FrameStats::OnSwap) and installs the frame clock the decision is keyed on (the HOOK clock, wwe2k14::g_guest_frame: SCENE_SKIPS.md section 2).
// Every hook of the scene tasks asks wwe2k14::scene_skip::Ours(Hook) FIRST; true = do the listed side effects and return without the original. The decision
// itself is scene_frame::Decide (one per frame, node_switch.h Board::Once): no hook decides on its own (tools\renderer\scene_skip_unit.cpp lints for it).
// Mode 0 (default) and 1: Ours() is false after one relaxed load, every hook calls the original, byte for byte.
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../frame_stats.h"  // wwe2k14::g_guest_frame
#include "../renderer_adapter/census_flush.h"   // S102 CEN1
#include "../renderer_game/scene_skip_core.h"

REXCVAR_DECLARE(int32_t, wwe2k14_rdr_scene);   // defined ONCE, in renderer_adapter/live.cpp (lane S / the spine); the lint greps all of src\ for exactly one definition

namespace wwe2k14::scene_skip {

using ::scene_skip::Hook;

// The hook clock: the guest frame the walks see. The stream frame holding the same walks is this + 1 (scene_skip_core.h kHookToStreamOffset).
inline uint64_t HookFrame() { return wwe2k14::g_guest_frame.load(std::memory_order_relaxed); }

// Installed at static initialisation (the pattern of draw_origin.h's g_d3_wcams_installed): the cvar's storage and the clock.
#if WWE2K14_STREAM()
inline const bool g_installed = (scene_frame::g_mode_cvar.store(&REXCVAR_GET(wwe2k14_rdr_scene), std::memory_order_relaxed), ::scene_skip::g_clock.store(&HookFrame, std::memory_order_relaxed), true);
#else
inline const bool g_installed = (::scene_skip::g_clock.store(&HookFrame, std::memory_order_relaxed), true);   // S103 OFFT: the mode is a constant (scene_frame::CurrentMode), no cvar to point at
#endif

// The one test every scene hook makes first.
inline bool Ours(Hook h) { return ::scene_skip::Ours(h); }

// The census line every 600 frames and at exit (Mode Ours only prints counts above 0): called by the first hook that crosses a 600-frame boundary.
inline void MaybeLog(Hook h) {
  if (scene_frame::CurrentMode() != scene_frame::Mode::Ours) return;
  static std::atomic<uint64_t> last{0};
  const uint64_t frame = HookFrame();
  uint64_t seen = last.load(std::memory_order_relaxed);
  static census_flush::Site cf_site;   // S102 CEN1: a flip phase end prints the census now
  if ((frame < seen + 600 && !census_flush::Due(cf_site)) || !last.compare_exchange_strong(seen, frame)) return;
  REXLOG_INFO("{} (hook {} at frame {}); {}", ::scene_skip::CensusLine(), ::scene_skip::HookName(h), (unsigned long long)frame, scene_frame::CensusLine());
}

}  // namespace wwe2k14::scene_skip
