// wwe2k14 - the precompile (Session 70, Jarrod: "every pipeline built before the game starts"). Default off.
//
// v0.2.6's per-draw shader specialisation (Session 66) means far more pipelines, and a pipeline the PC has not compiled
// before costs 15-150 ms on a 5950X and more on a laptop (the Intel tester: 76 ms on average). Compiled during an
// entrance they stall it: measured in Session 70 with cold driver caches and slowed compiles, v0.2.6 / v0.2.7 froze
// 4.6-4.7 s at the start of Brock's entrance (128 new pipelines in its first frames), v0.2.5 needed 8 there. The
// driver keeps what it compiled (NVIDIA: a cache keyed by the pipeline itself), so the cure is to make every pipeline
// once, before the game starts, using the whole CPU - after an update, a driver change or a setting that changes
// pipelines. The launcher runs the game in this mode first (src\launcher\launcher_main.cpp, "Preparing shaders").
//
//   --wwe2k14_precompile=FILE   the game boots as usual (with the player's own settings: the same pipelines as a Play),
//                               the SDK's gpu_native_precompile makes every stored NATIVE pipeline (the player's own
//                               store + the shipped list) at once on a worker per CPU thread, and the game writes its
//                               progress to FILE (JSON, every 10 frames: state starting / running / done / timeout,
//                               made, total, waiting, optimised, seconds) and quits when nothing is left. The stored
//                               pipelines of the emulated path are made before the first frame anyway (the SDK's boot,
//                               blocking).
//   --wwe2k14_precompile_optimise=true   (Session 70 item 5, off) also every shader's code not optimised yet is
//                               optimised (SPIRV-Tools, exact here) on all threads and stored in the player's cache
//                               (TITLE.spvopt); the game then makes its modules from it at no cost. This run's own
//                               pipelines are from the unoptimised code, so when it stored any ("optimised" > 0) the
//                               launcher runs it once more to make the optimised pipelines. Off: on the 4090 no
//                               measurable GPU gain and no faster compiles, for a precompile twice as long.
//   --wwe2k14_precompile_timeout_s=600   gives up (state timeout) after this long.
//
// Sound is muted (audio_mute) unless set on the command line. Nothing is written but FILE: the pipelines go to the
// driver's own cache (and the SDK's stores, as in any run).
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "input_script.h"      // g_request_quit
#include "kernel_overrides.h"  // g_vdswap_hook

REXCVAR_DEFINE_STRING(wwe2k14_precompile, "", "wwe2k14",
                      "Session 70: the precompile - every stored pipeline made before the game is played, progress "
                      "written to this file (JSON), then quit. The launcher runs it. See src/precompile.h");
REXCVAR_DEFINE_BOOL(wwe2k14_precompile_optimise, false, "wwe2k14",
                    "Session 70: the precompile also optimises every shader (SPIRV-Tools, exact) into the player's "
                    "cache and the game uses that code (a second precompile pass). Off: no measurable gain on the "
                    "4090; for integrated graphics");
REXCVAR_DEFINE_INT32(wwe2k14_precompile_timeout_s, 600, "wwe2k14",
                     "Session 70: the precompile gives up after this many seconds")
    .range(10, 7200);

namespace wwe2k14::precompile {

inline void (*g_prev_hook)(uint64_t) = nullptr;
inline bool g_done = false;
inline std::chrono::steady_clock::time_point g_t0;
inline std::string g_last;

inline void Write(const std::string& state, uint32_t made, uint32_t total, uint32_t waiting, uint32_t optimised = 0) {
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  const std::string json = "{\"state\": \"" + state + "\", \"made\": " + std::to_string(made) + ", \"total\": " +
                           std::to_string(total) + ", \"waiting\": " + std::to_string(waiting) +
                           ", \"optimised\": " + std::to_string(optimised) +
                           ", \"seconds\": " + std::to_string(int(secs * 10) / 10.0) + "}";
  if (json == g_last) return;
  // (Review: the command line is UTF-8 - u8path, or a user name like "José" writes to a mangled folder.)
  const std::filesystem::path path = std::filesystem::u8path(REXCVAR_GET(wwe2k14_precompile));
  std::filesystem::path part = path;
  part += ".part";
  {
    std::ofstream o(part, std::ios::binary | std::ios::trunc);
    o << json;
  }
  // (Whole file or nothing for the launcher's reader. Review: the rename fails while the launcher or a virus scanner
  // has the file open - the final state is tried again for up to a second, the others at the next update.)
  const bool final_state = state == "done" || state == "timeout";
  for (int i = 0; i < (final_state ? 20 : 1); ++i) {
    std::error_code ec;
    std::filesystem::rename(part, path, ec);
    if (!ec) {
      g_last = json;
      return;
    }
    if (final_state) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// "made M of T, waiting W, done D, optimised O" (the SDK's gpu_native_precompile_status) -> false when not written yet.
inline bool Parse(const std::string& s, uint32_t& made, uint32_t& total, uint32_t& waiting, bool& done,
                  uint32_t& optimised) {
  unsigned m = 0, t = 0, w = 0, d = 0, o = 0;
  if (std::sscanf(s.c_str(), "made %u of %u, waiting %u, done %u, optimised %u", &m, &t, &w, &d, &o) < 4) return false;
  made = m, total = t, waiting = w, done = d != 0, optimised = o;
  return true;
}

inline void OnSwap(uint64_t frame) {
  if (g_prev_hook) g_prev_hook(frame);
  if (g_done || frame % 10) return;
  uint32_t made = 0, total = 0, waiting = 0, optimised = 0;
  bool done = false;
  bool have = Parse(rex::cvar::GetFlagByName("gpu_native_precompile_status"), made, total, waiting, done, optimised);
  // (Without the native renderer there is nothing to make after the boot's own: done once the game draws. With it -
  // gpu_native not 0, the SDK's own rule - the SDK's status decides, also when the renderer turned itself off.)
  if (rex::cvar::GetFlagByName("gpu_native") == "0" && frame >= 120) done = true, have = true;
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  const bool timeout = secs > REXCVAR_GET(wwe2k14_precompile_timeout_s);
  if (!done && !timeout) {
    Write(have ? "running" : "starting", made, total, waiting, optimised);
    return;
  }
  g_done = true;
  Write(done ? "done" : "timeout", made, total, waiting, optimised);
  REXLOG_INFO("[wwe2k14][precompile] {}: {} of {} stored pipelines made, {} waiting, {} shaders optimised and stored, "
              "{:.1f} s (frame {}); quitting",
              done ? "done" : "TIMEOUT", made, total, waiting, optimised, secs, frame);
  if (g_request_quit) g_request_quit();
}

// OnPostSetup (before the first frame).
inline void Install() {
  if (REXCVAR_GET(wwe2k14_precompile).empty()) return;
  g_t0 = std::chrono::steady_clock::now();
  rex::cvar::SetFlagByName("gpu_native_precompile", "true");
  if (rex::cvar::GetFlagSource("audio_mute") == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName("audio_mute", "true");
  // The shader optimiser (SDK spirv_opt_store.h), --wwe2k14_precompile_optimise: the code not stored yet optimised on
  // all threads and stored; this run's own modules stay unoptimised, so the launcher runs a second pass when it stored
  // any ("optimised" > 0). Off by default (Session 70: exact, but no measurable gain on the 4090 - GPU within the
  // run-to-run spread, driver compile time unchanged - for a precompile twice as long; for an iGPU measurement).
  if (REXCVAR_GET(wwe2k14_precompile_optimise) &&
      rex::cvar::GetFlagSource("vulkan_spirv_optimize_collect") == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName("vulkan_spirv_optimize_collect", "true");
  Write("starting", 0, 0, 0);
  g_prev_hook = wwe2k14::g_vdswap_hook;
  wwe2k14::g_vdswap_hook = &OnSwap;
  REXLOG_INFO("[wwe2k14][precompile] armed: progress to {}", REXCVAR_GET(wwe2k14_precompile));
}

}  // namespace wwe2k14::precompile
