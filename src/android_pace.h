#pragma once
// wwe2k14 - Session 93, idea 14 (Android): steadier frames through present pacing, thermal awareness and the M5c pace gate.
// Every switch defaults off; with all of them off nothing here changes anything (the hooks return at once).
//
// 1. --present_display_timing (an SDK switch, rexglue-sdk-src ui/vulkan/vulkan_presenter.cpp, branch s93-pace): the presenter
//    asks for VK_GOOGLE_display_timing and presents each frame at "the last actual present + N refresh cycles" (N = the
//    display's rate / the game's rate: 60 fps on a 120 Hz panel = 2 cycles; a 90 Hz panel gives no whole number for 60 fps:
//    unpaced). It logs [pace] display timing lines itself. The game's part is here: it tells the presenter the game's frame
//    rate (SDK cvar present_target_fps) whenever the frame-rate word changes (frame_word.h), only while the switch is on.
// 2. --wwe2k14_android_thermal: src\android_thermal.h (the headroom read once a second, dynamic resolution stepped down).
// 3. --wwe2k14_android_pace_gate: sets the SDK's gpu_d3d_pace_gate (Session 92 M5c) under TRUE NATIVE (--wwe2k14_d3d_owned),
//    which also needs gpu_d3d_sync_direct and gpu_d3d_sync_events (TRUE NATIVE sets both). Android only.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdint>
#include <string>

#include "android_thermal.h"
#include "d3d_owned.h"  // wwe2k14_d3d_owned
#include "frame_word.h"

REXCVAR_DEFINE_BOOL(wwe2k14_android_pace_gate, false, "wwe2k14",
                    "Session 93 (Android): with TRUE NATIVE (--wwe2k14_d3d_owned) set the SDK's gpu_d3d_pace_gate (Session 92 "
                    "M5c: the flip handshake does not wait for the flip word; src/android_pace.h). Default off");

namespace wwe2k14::android_pace {

// OnPreSetup, right after d3d_owned::Apply(): piece 3, and the start line of each switch.
inline void Apply() {
  const bool timing = rex::cvar::GetFlagByName("present_display_timing") == "true";
  const bool thermal = REXCVAR_GET(wwe2k14_android_thermal);
  const bool gate = REXCVAR_GET(wwe2k14_android_pace_gate);
  if (!timing && !thermal && !gate) return;
  REXLOG_INFO("[pace] switches: present_display_timing={} wwe2k14_android_thermal={} wwe2k14_android_pace_gate={}", timing,
              thermal, gate);
  if (!gate) return;
#if defined(__ANDROID__)
  if (!REXCVAR_GET(wwe2k14_d3d_owned)) {
    REXLOG_INFO("[pace] pace gate: not set (needs TRUE NATIVE, --wwe2k14_d3d_owned)");
    return;
  }
  const bool direct = rex::cvar::GetFlagByName("gpu_d3d_sync_direct") == "true";
  const bool events = rex::cvar::GetFlagByName("gpu_d3d_sync_events") == "true";
  if (!direct || !events) {
    REXLOG_INFO("[pace] pace gate: not set (gpu_d3d_sync_direct={} gpu_d3d_sync_events={}: the gate needs both)", direct,
                events);
    return;
  }
  if (rex::cvar::GetFlagSource("gpu_d3d_pace_gate") == rex::cvar::Source::kDefault) {
    rex::cvar::SetFlagByName("gpu_d3d_pace_gate", "true");
    REXLOG_INFO("[pace] pace gate: gpu_d3d_pace_gate=true (TRUE NATIVE on Android)");
  } else {
    REXLOG_INFO("[pace] pace gate: gpu_d3d_pace_gate left as given on the command line ({})",
                rex::cvar::GetFlagByName("gpu_d3d_pace_gate"));
  }
#else
  REXLOG_INFO("[pace] pace gate: ignored (Android only)");
#endif
}

#if defined(__ANDROID__)
// Once per guest frame (frame_stats.h OnSwap).
inline void OnFrame() {
  static const bool timing = rex::cvar::GetFlagByName("present_display_timing") == "true";
  static const bool thermal = REXCVAR_GET(wwe2k14_android_thermal);
  if (!timing && !thermal) return;
  if (thermal) android_thermal::g_reader.Tick();
  if (timing) {
    // The presenter's N follows the game's frame rate (60, or 30 in entrances / with the 30 fps choice).
    static uint32_t published = 0;
    const uint32_t word = g_frame_word.load(std::memory_order_relaxed);
    if (word != published && word >= 25 && word <= 1000) {
      published = word;
      rex::cvar::SetFlagByName("present_target_fps", std::to_string(word));
    }
  }
}
#endif

}  // namespace wwe2k14::android_pace
