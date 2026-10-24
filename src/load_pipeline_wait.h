// wwe2k14 - no draw skipped while a match loads (Session 57, tester bug 1; Jarrod 2026-10-24: "Go for 1"). Default on.
//
// A draw whose pipeline is still being made is skipped (async_shader_compilation), unless it goes into a surface
// narrower than the screen (Session 55: those wait, they build textures the game keeps). Red patches are still
// reported (a GTX 1060, v0.2.1-native-s56-playtest), and every match load here skips ~270 screen-wide draws, ~200 of
// them in the first frames of the match scene (runs 3865-3873, cold cache): whether any of those build something the
// game keeps is not known (the [s57][skipped] log lines say from now on). So while an in-game load window is open
// (load_turbo.h: the pre-entrance load and the match load, on the loading screen where a pause costs nothing) and for
// --wwe2k14_load_pipeline_wait_hold_frames after it, no draw is skipped at all: the SDK's
// gpu_native_truth_pipeline_skip_min_pitch (NATIVE) and vulkan_pipeline_sync_offscreen_min_pitch (the emulated path)
// are raised to "never" and put back afterwards. During play nothing changes.
//
// Session 58: also for --wwe2k14_readback_char_hold_frames after a character file opens (create_readback.h). Some
// characters' attires are built on the GPU into a 2048x1024 texture on a 2080-wide surface, wider than the screen, so
// Session 55's width rule let its draws be skipped: 30 Years of WrestleMania loads Bobby Heenan in its menu (the match
// preview) and two draws into his attire were skipped there (runs 3887 / 3897, 0x1D285000); in the match he was red.
//
//   --wwe2k14_load_pipeline_wait=false   off (draws may be skipped during loads too, as before Session 57)
//
// Needs the load windows (--wwe2k14_load_turbo, default on). A width setting given on the command line is left alone.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdint>
#include <string>

#include "create_readback.h"  // InCharacterWindow
#include "input_script.h"     // g_pipeline_wait_frame_hook
#include "load_turbo.h"       // g_load_turbo_ingame_window

REXCVAR_DEFINE_BOOL(wwe2k14_load_pipeline_wait, true, "wwe2k14",
                    "Session 57: no draw is skipped while its pipeline is made during the in-game loads (the "
                    "pre-entrance and match loading screens) and shortly after: the screen-wide skip is raised to "
                    "never, then put back. See src/load_pipeline_wait.h");
REXCVAR_DEFINE_INT32(wwe2k14_load_pipeline_wait_hold_frames, 240, "wwe2k14",
                     "Session 57: frames after an in-game load window closes during which draws still wait (the load "
                     "window closes just before the loading screen fades out; the first scene frames make most of the "
                     "new pipelines)")
    .range(0, 3600);

namespace wwe2k14::load_pipeline_wait {

struct Setting {
  const char* name;
  const char* never;
  std::string saved;
};
inline Setting g_settings[] = {{"gpu_native_truth_pipeline_skip_min_pitch", "1024", {}},
                               {"vulkan_pipeline_sync_offscreen_min_pitch", "16384", {}}};
inline bool g_on = false;
inline uint64_t g_closed_frame = 0;  // the frame the last load window closed (0 = none yet)
inline bool g_was_open = false;

// VdSwap, every guest frame (the game thread).
inline void OnFrame(uint64_t frame) {
  const bool open =
      g_load_turbo_ingame_window().load(std::memory_order_relaxed) || create_readback::InCharacterWindow(frame);
  if (g_was_open && !open) g_closed_frame = frame;
  g_was_open = open;
  const uint64_t hold = uint64_t(REXCVAR_GET(wwe2k14_load_pipeline_wait_hold_frames));
  const bool on = open || (g_closed_frame != 0 && frame - g_closed_frame < hold);
  if (on == g_on) return;
  g_on = on;
  for (Setting& s : g_settings) {
    if (on) {
      s.saved = rex::cvar::GetFlagByName(s.name);
      rex::cvar::SetFlagByName(s.name, s.never);
    } else if (!s.saved.empty() && rex::cvar::GetFlagByName(s.name) == s.never) {
      rex::cvar::SetFlagByName(s.name, s.saved);  // (review: a value set live during the wait is kept)
    }
  }
  REXLOG_INFO("[wwe2k14][load_wait] {} frame={}",
              on ? "on: no draw skipped while its pipeline is made (an in-game load or a character file)"
                 : "off: screen-wide draws may be skipped again",
              frame);
}

// OnPostSetup.
inline void Install() {
  if (!REXCVAR_GET(wwe2k14_load_pipeline_wait)) return;
  for (const Setting& s : g_settings) {
    if (rex::cvar::GetFlagSource(s.name) != rex::cvar::Source::kDefault) {
      REXLOG_INFO("[wwe2k14][load_wait] {} set on the command line: the loads leave the skip widths alone", s.name);
      return;
    }
  }
  g_pipeline_wait_frame_hook = OnFrame;
}

}  // namespace wwe2k14::load_pipeline_wait
