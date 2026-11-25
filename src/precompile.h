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
#include "renderer_host/store_progress.h"   // S105 B3: the stream-free store's counts and the owner-mode done rule (no renderer includes)
#include "kernel_overrides.h"  // g_vdswap_hook

REXCVAR_DECLARE(std::string, wwe2k14_precompile);
REXCVAR_DECLARE(bool, wwe2k14_precompile_optimise);
REXCVAR_DECLARE(int32_t, wwe2k14_precompile_timeout_s);

namespace wwe2k14::precompile {

inline void (*g_prev_hook)(uint64_t) = nullptr;
inline bool g_done = false;
inline std::chrono::steady_clock::time_point g_t0;
inline std::string g_last;

// Session 93 (M6 step 2): the translation store's variants done of those queued (the SDK status line's ", m6 D of T"); the
// phone launcher shows them as "Preparing shaders D / T".
inline uint32_t g_m6_done = 0, g_m6_total = 0;
inline uint32_t g_store_unresolved = 0;   // S105 B3: stored pipelines of the stream-free host's store still waiting for a program at the end (logged with "prepared N of M")

inline void Write(const std::string& state, uint32_t made, uint32_t total, uint32_t waiting, uint32_t optimised = 0) {
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  const std::string json = "{\"state\": \"" + state + "\", \"made\": " + std::to_string(made) + ", \"total\": " +
                           std::to_string(total) + ", \"waiting\": " + std::to_string(waiting) +
                           ", \"optimised\": " + std::to_string(optimised) +
                           ", \"m6_done\": " + std::to_string(g_m6_done) + ", \"m6_total\": " + std::to_string(g_m6_total) +
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
  // (Session 93: the SDK's M6 tail ", m6 D of T", when the SDK has it.)
  unsigned md = 0, mt = 0;
  const size_t at = s.find(", m6 ");
  if (at != std::string::npos && std::sscanf(s.c_str() + at, ", m6 %u of %u", &md, &mt) == 2) g_m6_done = md, g_m6_total = mt;
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
  // (S105 B3: not in owner mode: there the stream-free host's store decides, below; gpu_native is 0 there too and this line ended the boot at frame 120.)
  const bool owner = rex::cvar::GetFlagByName("wwe2k14_rdr_owner") == "true";
  if (!owner && rex::cvar::GetFlagByName("gpu_native") == "0" && frame >= 120) done = true, have = true;
  // Session 100: in owner mode the new renderer draws and the old one gets no work (strip-360 B6 skips its submits), so the old path's
  // stored pipelines never progress: the precompile never ended (Jarrod's phone test, 2026-11-11: "shader pipeline build forever,
  // requiring you to manually quit"). Nothing of the old path to make: done once the game draws, as without the native renderer.
  // S105 B3: that rule called the boot done at frame 120 with "0 of 0" while the player's stored list (the NEW renderer's pipelines) was still unbuilt: the first match frames then
  // built them cold (371-518 ms freezes on the Surface). Now the stream-free host's own store decides: done at frame >= 120 once it has nothing left it can try (made / total from its counts).
  if (owner) {
    namespace sp = wwe2k14::scene_host::store_progress;
    const bool pub = sp::g_progress.published.load(std::memory_order_acquire);   // F3: published first (acquire), then the counts it covers
    const uint32_t left = sp::g_progress.left.load(std::memory_order_relaxed);
    if (sp::OwnerPrecompileDone(frame, sp::g_progress.store_expected.load(std::memory_order_relaxed), pub, left)) done = true, have = true;
    if (pub) {
      made = sp::g_progress.made.load(std::memory_order_relaxed), total = sp::g_progress.total.load(std::memory_order_relaxed), waiting = left;
      g_store_unresolved = sp::g_progress.unresolved.load(std::memory_order_relaxed), have = true;
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  const bool timeout = secs > REXCVAR_GET(wwe2k14_precompile_timeout_s);
  if (!done && !timeout) {
    Write(have ? "running" : "starting", made, total, waiting, optimised);
    return;
  }
  g_done = true;
  // F1: in owner mode the host saves the store BEFORE "done" is written (the phone's activity ends the process within 300 ms of it). The host serves the request from its poll.
  if (owner && !wwe2k14::scene_host::store_progress::RequestFlushAndWait(wwe2k14::scene_host::store_progress::g_progress, 3000))
    REXLOG_INFO("[wwe2k14][precompile] the store flush was not acknowledged within 3 s");
  Write(done ? "done" : "timeout", made, total, waiting, optimised);
  REXLOG_INFO("[wwe2k14][precompile] {}: {} of {} stored pipelines made, {} waiting, {} shaders optimised and stored, "
              "{:.1f} s (frame {}); quitting",
              done ? "done" : "TIMEOUT", made, total, waiting, optimised, secs, frame);
  if (owner) REXLOG_INFO("[wwe2k14][precompile] prepared {} of {} stored pipelines of the new renderer ({} still waiting for a program)", made, total, g_store_unresolved);   // S105 B3
#ifndef __ANDROID__
  if (g_request_quit) g_request_quit();
#else
  // Session 93 (M6 step 2): on the phone the game's activity watches the progress file and ends the process itself (an app
  // shutdown on Android is the old exit crash, ANDROID_PLAN known issues): the final state is written, the game idles
  // until it is ended.
#endif
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
