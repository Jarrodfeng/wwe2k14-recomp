// wwe2k14 - `--wwe2k14_launch=@mode:<name>` (and `@cas`): any mode of src/mode_table.h opened from the idle root main menu with no menu input
// (S110 EXITMAP, docs\research\s110\EXITMAP.md; generalises S110 CASENTRY's @cas).
//
// The game's own way in: a menu leaf (or its screen-controller mode, after its checks) calls sub_8283CFF0(SM, exit); the main menu's dispatcher
// sub_82833440 switches on the exit word 0x83929434 and builds the mode's sequence. We make that same exit request at the idle main menu, as the
// @quick launch requests exit 1 (src/scenario.h FireLaunch) and the 30 Years start exit 3 (src/wm30_boot.h Exit), after the menu path's own
// refusals: the SmartMenu's enter gates on the records a player presses (src/launch_core.h Gates), then the screen controller's test for the
// row (RunCheck). Writes ONE thing: the exit request (and, with --wwe2k14_mode_pick on @mode:moveset, the move-set controller's own preselect
// word [G+16], after the picker's own test: src/mode_pick.h, S110 MODEPICK). TEST / TOOLING PATH: reached only by the switch.
//
// Log: "<value>: exit N -> sequence S (...); waiting for the idle main menu", "<value>: enter gates pass (screen C by records ...)",
// "[wwe2k14][launch] <value>: exit N sent at frame F (...)", then "<value>: sequence S is current at frame F (K frames after the exit)"
// (tools/scene/cas_sweep.py reads the @cas "exit 8 sent at frame F" line, unchanged from S110 CASENTRY; the first line and the refusal
// wording are new), or a WARN: refused / no such mode / no sequence S 900 frames after the exit.
//
// Game thread only: called from the owner update (src/cas_launch.h Tick, before the game's own update of the frame).
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "guest_call.h"   // Caller
#include "launch_core.h"  // MainMenuIdle, RunCheck
#include "mode_table.h"   // kModes, FromLaunch
#include "mode_pick.h"    // S110 MODEPICK: Prepare (the pick before the exit), Watch (its arrival line)
#include "scenario.h"     // U32, SequenceId, kSmartMenuPtr, kMenuSelection, wwe2k14_launch(_idle_frames), sub_8283CFF0

namespace wwe2k14::mode_launch {

constexpr uint64_t kWarnFrames = 900;

enum class Phase { kOff, kWaiting, kSent, kDone };
// Game thread only (the owner update), so no locking.
inline Phase g_phase = Phase::kOff;
inline const Mode* g_mode = nullptr;
inline std::string g_name;
inline uint64_t g_idle = 0, g_sent_frame = 0;

inline std::string KnownNames() {
  std::string s;
  for (const Mode& m : kModes) {
    if (!s.empty()) s += ", ";
    s += std::string(m.name);
  }
  return s;
}

// Once per guest frame. Off: one string read per frame until a launch value is seen, then nothing.
inline void Step(PPCContext& ctx, uint8_t* base, uint64_t frame) {
  using namespace scenario;
  mode_pick::Watch(frame);  // S110 MODEPICK: one bool test while no pick is pending; the arrival comes after this launch is kDone
  if (g_phase == Phase::kDone) return;
  if (g_phase == Phase::kOff) {
    const std::string launch = REXCVAR_GET(wwe2k14_launch);
    if (launch.empty()) return;
    if (!IsModeLaunch(launch)) {
      g_phase = Phase::kDone;  // another launch kind (src/scenario.h LaunchTick serves it)
      return;
    }
    g_name = launch;
    g_mode = FromLaunch(launch);
    if (!g_mode) {
      REXLOG_WARN("[wwe2k14][launch] {}: no such mode (known: {}); nothing sent", g_name, KnownNames());
      g_phase = Phase::kDone;
      return;
    }
    REXLOG_INFO("[wwe2k14][launch] {}: exit {} -> sequence {} ({}; check: {}); waiting for the idle main menu", g_name, g_mode->exit, g_mode->seq,
                std::string(g_mode->path), CheckName(g_mode->check));
    g_phase = Phase::kWaiting;
    return;
  }
  const uint32_t seq = SequenceId();
  if (g_phase == Phase::kWaiting) {
    g_idle = launch_core::MainMenuIdle() ? g_idle + 1 : 0;
    if (g_idle < uint64_t(std::max<int64_t>(1, REXCVAR_GET(wwe2k14_launch_idle_frames)))) return;
    const launch_core::Verdict gate = launch_core::Gates(ctx, base, g_mode->screen);
    if (!gate.ok) {
      REXLOG_WARN("[wwe2k14][launch] {}: refused, {}: the game's own menu path refuses here; exit {} not sent", g_name, gate.detail, g_mode->exit);
      g_phase = Phase::kDone;
      return;
    }
    REXLOG_INFO("[wwe2k14][launch] {}: enter gates pass ({})", g_name, gate.detail);
    const launch_core::Verdict v = launch_core::RunCheck(ctx, base, g_mode->check);
    if (!v.ok) {
      REXLOG_WARN("[wwe2k14][launch] {}: refused, {}: the game's own menu path refuses here; exit {} not sent", g_name, v.detail, g_mode->exit);
      g_phase = Phase::kDone;
      return;
    }
    const launch_core::Verdict pick = mode_pick::Prepare(ctx, base, *g_mode, g_name, frame);  // S110 MODEPICK: "" detail when no pick is set
    if (!pick.ok) {
      REXLOG_WARN("[wwe2k14][launch] {}: refused, {}; exit {} not sent", g_name, pick.detail, g_mode->exit);
      g_phase = Phase::kDone;
      return;
    }
    const uint32_t sm = U32(kSmartMenuPtr);
    scene_director::Caller call(ctx, base);
    call(sub_8283CFF0, {uint64_t(sm), uint64_t(g_mode->exit)});
    g_sent_frame = frame;
    g_phase = Phase::kSent;
    REXLOG_INFO("[wwe2k14][launch] {}: exit {} sent at frame {} (0x83929434 = {}; {})", g_name, g_mode->exit, frame, U32(kMenuSelection),
                pick.detail.empty() ? v.detail : v.detail + "; " + pick.detail);
    return;
  }
  if (seq == g_mode->seq) {
    REXLOG_INFO("[wwe2k14][launch] {}: sequence {} is current at frame {} ({} frames after the exit)", g_name, g_mode->seq, frame, frame - g_sent_frame);
    g_phase = Phase::kDone;
  } else if (frame - g_sent_frame == kWarnFrames) {
    REXLOG_WARN("[wwe2k14][launch] {}: no sequence {} {} frames after exit {} (sequence {}, exit action {}, SmartMenu {:08X})", g_name, g_mode->seq,
                kWarnFrames, g_mode->exit, seq, U32(kMenuSelection), U32(kSmartMenuPtr));
  }
}

}  // namespace wwe2k14::mode_launch
