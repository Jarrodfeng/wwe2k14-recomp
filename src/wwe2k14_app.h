// wwe2k14 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

#include "crash_handler.h"
#include "gpu_timeline.h"
#include "pacing_hooks.h"
#include "tiling_hooks.h"
#include "kernel_overrides.h"
#include "missing_function_scan.h"
#include "thread_placement.h"
#include "image_dump.h"
#include "determinism.h"  // --wwe2k14_fixed_seed (Session 8 task 8a)
#include "slow_frame.h"   // --wwe2k14_slowframe_ms (Session 8 task 12, default off)
#include "inspector.h"    // --wwe2k14_inspector (Session 8 task 15, default off)
#include "barrier_probe.h"  // --wwe2k14_barrier_probe / _names (Session 8 task B, default off)
#include "fp_probe.h"       // --wwe2k14_fp_probe (Session 8 task A, default off)
#include "decode_probe.h"   // --wwe2k14_decode_probe / _shadow (Session 8 task 3, default off)
#include "lz4_probe.h"      // --wwe2k14_lz4_probe (Session 15 extension E1, default off)
#include "lzx_probe.h"      // --wwe2k14_native_lzx (default on) / _verify / _lzx_probe / _corpus (Session 12)
#include "load_probe.h"     // --wwe2k14_load_probe (Session 12, default off)
#include "scenario.h"       // --wwe2k14_scenario_observe (Session 9 Goal 1 step 1, default off)
#include "cinecam.h"        // --wwe2k14_cinecam_* (Session 13, M6 CineCam, default off)
#include "flow_probe.h"     // --wwe2k14_flow_probe (Session 14, Rumble diagnosis, default off)
#include "reshoot.h"        // --wwe2k14_reshoot (Session 16 item 2, default off)
#include "runin.h"          // --wwe2k14_runin (Session 16 item 9 experiment, default off)
#include "director.h"       // --wwe2k14_director (Session 17 Track 3, default off)
#include "tron_shim.h"
#include "logo_import.h"    // --wwe2k14_logo_import (Session 18, logo importer v0, default off)
#include "audio_capture.h"  // --wwe2k14_audio_capture (Session 18, sound for re-shoots, default off)
#include "tron_export.h"    // --wwe2k14_tron_export (Session 17 launcher previews, default off)      // MP4 / MOV / MKV titantrons via Media Foundation (Session 17 item 1B; needs --wwe2k14_mods)
#include "motion_fix.h"     // --wwe2k14_fix_secondary_motion (Session 27.5: Cena's title at 60 fps, default off)
#include "gfx_menu.h"       // GRAPHICS menu in the Options (Session 28)
#include "dlc.h"            // --wwe2k14_dlc: the player's DLC packs (Session 32, default off)
#include "background_speed.h"  // --wwe2k14_background_full_speed (Session 32, default off; run_list turns it on)
#include "live_control.h"   // --wwe2k14_live (Session 29: live control channel, default off)
#include "check_prompt.h"   // input-script `check TEXT` (Session 29: on-screen question, harness runs only)
#include "vulkan_layers.h"  // Session 36 (GitHub issue 1): Overwolf's Vulkan layers kept out (they crashed the game)
#include "gpu_pick.h"       // Session 31: the dedicated GPU by default, --wwe2k14_gpu=NAME (the launcher's chooser)
#include "light_renderer.h" // Session 37: --wwe2k14_light_renderer (weak GPUs), --wwe2k14_fbo_fallback_fix (default off)
#include "photo_mode.h"     // --wwe2k14_photo_mode (Session 33 item 4, default off)
#include "ultrawide.h"      // --wwe2k14_aspect (Session 33 item 6, default 16:9 = off)
#include "wm30_jump.h"      // --wwe2k14_wm30 (Session 34: jump into a 30 Years of WrestleMania match, default off)
#include "upscale_guard.h"  // Session 37: a failed FSR turns itself off with a notice (with --wwe2k14_fsr)
#include "direct_start.h"   // Session 53: a release started without the launcher (hand-off, settings file, message)
#include "keybinds_save.h"  // Session 53: the keyboard binds saved in the save folder (--wwe2k14_save_keybinds)

// Debug: log big-endian guest words at launch, e.g. --wwe2k14_dump_words=0x835A97E4:11,0x82000000:4
// (used to read jump tables and pointer tables that live inside the encrypted XEX).
REXCVAR_DEFINE_STRING(wwe2k14_dump_words, "", "wwe2k14",
                      "Comma-separated addr:count pairs of guest words to log at launch");

// Xenia's known-good 2K14 setting is gpu = "vulkan" (CLAUDE.md); its D3D12 path faults on this title
// (PLAYBOOK: nvlddmkm event 153 -> "Graphics device lost"). Needs an SDK built with REXGLUE_USE_VULKAN=ON;
// falls back to "any" if the plugin lacks the requested backend.
REXCVAR_DEFINE_STRING(wwe2k14_gpu_backend, "vulkan", "wwe2k14",
                      "GPU backend for the xenos plugin: vulkan, d3d12 or any");

// ReXGlue deliberately leaves cache: (the 360 HDD cache partition) unmounted (system/runtime.cpp SetupVfs),
// so 2K14's opens of cache:\EditEx.cac and cache:\CAR.cac fail. Experiment for the Create a Logo ->
// Lettering crash (PLAYBOOK session 3): mount a host folder there instead.
// Session 7 prep: the same for the defaults added after Session 6 (see OnPreSetup).
// Session 22 (2026-10-15, Jarrod: "ready to default" after his flicker check of the 165 bundle): the GPU winners of
// Sessions 21-22 (see OnPreSetup).
REXCVAR_DEFINE_BOOL(wwe2k14_s22_defaults, true, "wwe2k14",
                    "Apply the Session 21-22 GPU defaults (no-op draw skip, upload hoist, one present per game frame, "
                    "early texture loads, direct depth texture loads); false = Session 20 behaviour");
// Session 23 (2026-10-15, Jarrod: make them defaults after reading SESSION23_SUMMARY.md): repeat draws (see OnPreSetup).
REXCVAR_DEFINE_BOOL(wwe2k14_s23_defaults, true, "wwe2k14",
                    "Apply the Session 23 GPU defaults (repeat draws: fast path + instanced merge of the crowd "
                    "shaders); false = Session 22 behaviour");
REXCVAR_DEFINE_BOOL(wwe2k14_s7_defaults, true, "wwe2k14",
                    "Apply the Session 7 GPU command-processor optimisation defaults (false = Session 6 behaviour)");

// Session 7b: one switch for 1440p 60 fps (see OnPreSetup).
// Session 8 (2026-10-09): ON by default, Jarrod's decision after his visual check of review batch 1 (edges, crowd,
// motion, menus, Create a Wrestler all clean at 1440p/4K). =false restores the original look (720p, the game's 4x MSAA,
// 30 fps entrances). Later: follow the monitor's native resolution.
REXCVAR_DEFINE_BOOL(wwe2k14_preset_1440p60, true, "wwe2k14",
                    "1440p 60 fps (default on): no MSAA, resolution_scale=2, 60 fps entrances, skip unchanged "
                    "re-uploads; =false restores the original 720p / 4x MSAA / 30 fps entrance look");
// Session 7c: the same at 4K (resolution_scale=3).
REXCVAR_DEFINE_BOOL(wwe2k14_preset_4k60, false, "wwe2k14",
                    "4K 60 fps: as wwe2k14_preset_1440p60, but resolution_scale=3");
// Session 41 (low-spec step 2, docs\native\LOWSPEC_PLAN.md): one switch for integrated graphics (see OnPreSetup).
REXCVAR_DEFINE_BOOL(wwe2k14_preset_low, false, "wwe2k14",
                    "Session 41: the LOW preset for integrated graphics: 720p (resolution_scale=1, no MSAA), the "
                    "native renderer (gpu_native=2; the SDK turns it off without the interlock path, where the light "
                    "renderer's fixes apply), exact cheap clears (gpu_fast_clear), 30 fps (game step 30 in matches, "
                    "stock 30 fps entrances), a shorter vblank spin. Default off; "
                    "the launcher's LOW choice and its first-run wizard on integrated graphics set it");
// Session 43 (stretch S4, docs\native\STEAM_DECK.md): groundwork for a Steam Deck preset (see OnPreSetup). Default
// off; nothing detects the Deck yet. Untested on a Deck.
REXCVAR_DEFINE_BOOL(wwe2k14_preset_deck, false, "wwe2k14",
                    "Session 43 (groundwork, untested on a Deck): the Steam Deck preset - 1280x720 (resolution_scale=1, "
                    "no MSAA; letterboxed in the Deck's 1280x800), the native renderer (gpu_native=2; RADV under Proton "
                    "has fragment shader interlock), exact cheap clears, 60 fps entrances and matches (the game's own "
                    "steps), a shorter vblank spin. Default off");
// Session 22 item 1: one switch for a 165 Hz monitor (default off until Jarrod's flicker check; see OnPreSetup).
REXCVAR_DEFINE_BOOL(wwe2k14_preset_165, false, "wwe2k14",
                    "165 Hz setup (experiment, default off): Session 21's GPU winners, the match at 165 game steps "
                    "per second and entrances at 144 (step_vblank), mailbox presentation");

// Session 6: one switch for all of this session's command-processor optimisation defaults (see OnPreSetup).
REXCVAR_DEFINE_BOOL(wwe2k14_s6_defaults, true, "wwe2k14",
                    "Apply the Session 6 GPU command-processor optimisation defaults (false = Session 5 behaviour)");

REXCVAR_DEFINE_BOOL(wwe2k14_mount_cache, false, "wwe2k14",
                    "Mount <user data>/cache_partition as the Xbox 360 HDD cache partition (cache:)");

class Wwe2k14App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Wwe2k14App>(new Wwe2k14App(ctx, "wwe2k14",
        PPCImageConfig));
  }

  // Dev-layout default: with no --game_data_root, search upwards from the exe
  // (out/build/<preset>/) for game_data/extracted/default.xex.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Keep far more log history than ReXGlue's default (5 MB files): GPU warnings can fill 5 MB in ~13 s,
    // which rotated away the whole match before a crash (wwe2k14_014). This is the last hook before
    // logging is configured. Command-line values still win; a config-file value would not.
    // Session 26: this build's code is recompiled from the disc's default.xex (0.0.0.2, no title update). A
    // default.xexp (title update) next to it would patch the data under unpatched code: ReXGlue logs and ignores it.
    for (auto [name, value] : {std::pair{"log_max_file_size_mb", "100"}, std::pair{"log_max_files", "20"},
                               std::pair{"xex_apply_patch", "false"}}) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    }

    // --game_data_root may be a folder or (Session 26) a disc image file (.iso); ReXGlue mounts either as GAME:.
    if (!paths.game_data_root.empty()) return;
    auto dir = rex::filesystem::GetExecutableFolder();
    for (int depth = 0; depth < 6; ++depth) {
      auto candidate = dir / "game_data" / "extracted";
      if (std::filesystem::is_regular_file(candidate / "default.xex")) {
        paths.game_data_root = candidate;
        return;
      }
      if (dir == dir.parent_path()) break;
      dir = dir.parent_path();
    }
    wwe2k14::direct_start::Configure(paths);  // Session 53: a release started directly (no game files found yet)
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    wwe2k14::vulkan_layers::Apply();  // Session 36: before anything starts Vulkan (GitHub issue 1)
    // Session 16 item 1: replay mode preset and a playback file's seed / settings, before anything reads them.
    wwe2k14::gfx_menu::ApplyLauncherSettings();  // Session 28: graphics settings from the launcher's file (release builds)
    if (REXCVAR_GET(wwe2k14_use_launcher_settings))  // Session 32: the DLC switches too
      wwe2k14::dlc::ApplyLauncherSettings(wwe2k14::gfx_menu::SettingsFile());
    wwe2k14::reshoot::PreSetup();  // sets --wwe2k14_replay_play etc. first (no-op unless --wwe2k14_reshoot)
    wwe2k14::director::PreSetup();  // Session 17: scenario / seed / replay mode from a director script (if any)
    wwe2k14::replay::g_replay.PreSetup();
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";

    // Load the GPU plugin here (the SDK would do the same right after this hook)
    // so its cvars are registered before graphics initialises.
    if (!config.graphics) {
      const std::string backend = REXCVAR_GET(wwe2k14_gpu_backend);
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      if (!config.graphics && backend != "any") {
        REXLOG_WARN("[wwe2k14] GPU backend '{}' unavailable in plugin '{}'; falling back to 'any'", backend,
                    config.gpu_plugin);
        config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "any");
      }
    }

    // readback_resolve: Xenia's known-good 2K14 setting is "fast" (CLAUDE.md: fixes outfits there), and this app
    // used to force it. A/B 2026-10-07 (Jarrod, wwe2k14_054 vs 056): "fast" more than doubles the GPU command
    // processor thread's load (Brock's entrance 57-85% vs 26-38%), dropping entrances from 30 to ~24 fps and
    // gameplay from 60 to 44-55; with ReXGlue's default "none", outfits were all correct (Brock, Bryan, referee,
    // crowd, Cena's shirt/cap/belt, Punk's hooded jacket). So we no longer override it. If something that reads
    // rendered images on the CPU breaks (thumbnails, saved logos...), try --readback_resolve=some or =fast.

    // 2K14 sets the "invalid" type on many texture fetch constants; with the default (false) the texture
    // cache skips binding them (src/graphics/pipeline/texture/cache.cpp). A/B test 2026-10-07 (Jarrod):
    // true fixes the dark arena/background and makes depth of field work. Only applied if left at default.
    if (rex::cvar::GetFlagSource("gpu_allow_invalid_fetch_constants") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
    }

    // Vulkan's default host-render-target path loses depth (crowd drawn over CM Punk) and flashes black.
    // A/B 2026-10-07 (Jarrod, wwe2k14_022): "fsi" (exact EDRAM emulation via fragment shader interlock)
    // fixes both. Only applied if left at default.
    // vulkan_gpu_crash_diagnostics comes from our SDK patch (patches/rexglue-sdk/): on device loss it logs
    // the driver's fault report and the last draws the GPU reached.
    // gpu_wait_reg_mem_spin (our SDK patch): the GPU thread yield-spins in WAIT_REG_MEM instead of sleeping
    // ~1-2 ms per poll; it waits once per frame for the game's CPU (memory 0x137FE006). A/B 2026-10-07
    // (Jarrod, wwe2k14_062 vs 063): Brock's entrance 28.7 -> 30.2 fps, frames >= 45 ms 10.0% -> 0.1%.
    // Cost: that thread uses a full core while waiting.
    // vulkan_async_submit + vulkan_submit_on_wait (our SDK patch, Session 5): in heavy shots ReXGlue's GPU
    // command thread needed 35-41 ms per frame against the 33.3 ms the game's 30 fps vblank pacing allows
    // (timeline, wwe2k14_066), ~7 ms of it replaying recorded commands into Vulkan. A separate submit thread now
    // does that. A/B (Jarrod + logs), Bryan's entrance has two camera variants (X/Y, told apart by GPU kicks per
    // frame), so compare within one: variant X, frames >= 45 ms 133 -> 37 (wwe2k14_066 vs 070, together with two
    // small SDK fixes); trons, models, outfits fine; his entrance sound effects almost in sync.
    // vulkan_deferred_descriptor_writes (same patch): the per-draw vkUpdateDescriptorSets also runs on the submit
    // thread. Variant X: 37 -> 9 (070 vs 074, CP busy -4.4%); variant Y: 78 -> 1 with everything (068 vs 073).
    // GPU-thread work per frame in his ramp pan (073): 27.5 ms average, 31.2 max (was 37-41 ms at the start of
    // Session 5). Jarrod: "almost flawless".
    // Session 37: --wwe2k14_light_renderer (default off) sets the host render target path and its fixes first.
    wwe2k14::light::Configure();
    for (auto [name, value] : {std::pair{"render_target_path_vulkan", "fsi"},
                               std::pair{"vulkan_gpu_crash_diagnostics", "true"},
                               std::pair{"gpu_wait_reg_mem_spin", "true"},
                               std::pair{"vulkan_async_submit", "true"},
                               std::pair{"vulkan_submit_on_wait", "true"},
                               std::pair{"vulkan_deferred_descriptor_writes", "true"}}) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    }

    // Session 6 (2026-10-08): command-processor optimisations from our SDK patch, each with a shadow-compare
    // (_verify) setting that found no difference on the CH103 route (runs 089, 098, 099; CP2's only mismatches were
    // the benign race it is designed for) and an in-run A/B win on the same frames (PLAYBOOK Session 6). Together:
    // CP work per frame -38% in CH103's heavy stretch (Undertaker, WrestleMania Moment arena), -32% in the match.
    // --wwe2k14_s6_defaults=false restores Session 5 behaviour in one flag (for A/B or if something looks wrong);
    // each setting can also be turned off on its own (--name=false). Only applied if left at default.
    if (REXCVAR_GET(wwe2k14_s6_defaults)) {
      for (auto [name, value] : {std::pair{"gpu_type0_bulk_writes", "true"},            // CP1: type-0 ranges in bulk
                                 std::pair{"gpu_type0_block_split", "true"},            // CP1B: split mixed ranges
                                 std::pair{"gpu_request_ranges_fast", "true"},          // CP2: no alloc, lock-free check
                                 std::pair{"vulkan_reuse_texture_bindings", "true"},    // CP5: reuse texture sets
                                 std::pair{"vulkan_sampler_param_cache", "true"},       // CP6: sampler parameters
                                 std::pair{"vulkan_constants_dynamic_offsets", "true"}, // CP7: needs a restart
                                 std::pair{"gpu_rt_cache_skip_unchanged", "true"},      // CP8: RT cache (FSI)
                                 std::pair{"vulkan_system_constants_skip", "true"},     // CP9: system constants
                                 std::pair{"vulkan_viewport_info_cache", "true"},       // viewport info cache
                                 std::pair{"vulkan_vertex_residency_cache_off", "true"},// drop the stale-prone cache
                                 std::pair{"vulkan_async_swap", "true"},                // DRAIN: no swap-time drain
                                 // H1: no command-processor wait on background pipeline compiles. Empty shader
                                 // cache (112 vs 113): 11 hitches >= 100 ms (5.1 s, worst 2.4 s) -> 2 (0.2 s);
                                 // a draw whose pipeline isn't ready holds its frame (shown again) instead.
                                 std::pair{"vulkan_pipeline_no_submit_wait", "true"},
                                 std::pair{"gpu_vblank_precise_timer", "true"}}) {      // O1: on-time guest vblanks
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
    }

    // Session 7 prep (2026-10-08, unattended): further command-processor defaults, same rules (shadow-compare clean,
    // in-run A/B win; docs/session7_prep_notes.md). --wwe2k14_s7_defaults=false turns only these off.
    if (REXCVAR_GET(wwe2k14_s7_defaults)) {
      for (auto [name, value] : {// TEXREQ: skip unchanged texture requests. Verify 0 mismatches (run 127); on top of
                                 // the Session 6 defaults CH103 heavy -1.1 ms, rest -0.5 ms per frame (run 128).
                                 std::pair{"gpu_request_textures_skip_unchanged", "true"},
                                 // UPSKIP (Session 7b): skip re-uploads of shared-memory pages whose bytes equal
                                 // what the GPU buffer holds (hash per page). Verify (GPU readback compare) 0
                                 // mismatches at default settings (run 159) and at 1440p (run 152); in-run A/B at
                                 // default settings: host GPU -4.6 ms per frame, CP unchanged, submit thread
                                 // -0.5 ms (run 158).
                                 std::pair{"gpu_upload_skip_unchanged", "true"},
                                 // TEXDIRECT (Session 7c): at resolution_scale > 1, textures produced by a resolve
                                 // are loaded by a compute shader that writes the image directly (no scratch buffer
                                 // + copy). Verify (old path vs direct image, byte compare) 0 mismatches in ~31,000
                                 // comparisons at 4K (run 166); in-run A/B at 4K: host GPU -3.4 ms per frame, CP
                                 // unchanged (run 167). No effect at resolution_scale 1 (no scaled loads).
                                 std::pair{"vulkan_texture_load_direct", "true"}}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
    }

    // Session 24: --wwe2k14_dlss picks the render scale from its mode (first, so the presets below leave it alone).
    wwe2k14::dlaa::ConfigureScale();

    // Session 41 (low-spec step 2): --wwe2k14_preset_low, the launcher's LOW choice for integrated graphics (set first,
    // so the 1440p preset below keeps only its no-MSAA / upload / texture-load parts). The frame rate is the launcher's
    // FRAME RATE 30 (gfx_settings.h, frame_rate 3): entrance_60fps off makes entrances the game's own 30 fps (word 30;
    // step 30 maps a stock 30 entrance back to 30, step_rate.h), matches step 30. gpu_native 2 needs the interlock
    // path: the SDK turns it off itself on the light renderer and on cards without interlock (Session 39, 298dd3f).
    // Each setting only if left at default.
    // Session 51 (Jarrod, 2026-10-22: "NATIVE for everyone"): the renderer part is the NATIVE renderer's own list
    // (gfx_settings.h NativeRendererFlags, the same the launcher's NATIVE sets); LOW adds only its quality part.
    if (REXCVAR_GET(wwe2k14_preset_low)) {
      std::vector<std::pair<std::string, std::string>> low = {
          {"resolution_scale", "1"},           // 1280x720
          {"wwe2k14_entrance_60fps", "false"},  // stock 30 fps entrances
          {"wwe2k14_step_hz", "30"},            // matches at the game's 30 fps mode
          {"wwe2k14_step_hz_entrance", "60"},   // (= unchanged for a 30 entrance)
          // Laptops: the precise vblank timer's final spin 1000 -> 250 us (run 2723: late mean 0 -> 45 us, max 675 us;
          // frame pacing unchanged: p99 17.7 vs 18.0 ms).
          {"gpu_vblank_precise_timer_spin_us", "250"},
          // Session 44 (Jarrod, 2026-10-21: "direct loads only" for the playtest build): direct 1x texture loads for
          // resolve outputs - exact (~57,000 loads verified, Session 43), GPU -5..-10% on LOW and -7..-15% on the light
          // renderer (runs 2819 / 2821).
          {"vulkan_texture_load_direct_unscaled", "true"}};
      for (const auto& flag : wwe2k14::gfxs::NativeRendererFlags()) low.push_back(flag);
      for (const auto& [name, value] : low) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
      REXLOG_INFO("[wwe2k14][preset_low] LOW preset: resolution_scale={}, entrance_60fps={}, step_hz={} (entrances {}), "
                  "gpu_native={}, vblank spin {} us, fast clears {}, direct 1x texture loads {}, truth frames {}, GPR "
                  "skip {}, half2 {}, lean draws {}, geometry trust {}",
                  rex::cvar::GetFlagByName("resolution_scale"), rex::cvar::GetFlagByName("wwe2k14_entrance_60fps"),
                  rex::cvar::GetFlagByName("wwe2k14_step_hz"), rex::cvar::GetFlagByName("wwe2k14_step_hz_entrance"),
                  rex::cvar::GetFlagByName("gpu_native"), rex::cvar::GetFlagByName("gpu_vblank_precise_timer_spin_us"),
                  rex::cvar::GetFlagByName("gpu_fast_clear"),
                  rex::cvar::GetFlagByName("vulkan_texture_load_direct_unscaled"),
                  rex::cvar::GetFlagByName("gpu_native_truth"),
                  rex::cvar::GetFlagByName("gpu_native_half2_skip_gpr_draws"),
                  rex::cvar::GetFlagByName("gpu_native_half2"), rex::cvar::GetFlagByName("gpu_native_lean_draws"),
                  rex::cvar::GetFlagByName("gpu_native_trust_geometry_pages"));
    }

    // Session 43 (stretch S4): --wwe2k14_preset_deck (docs\native\STEAM_DECK.md): as LOW, but 60 fps (the Deck's GPU is
    // ~4x a UHD 620; its screen is 1280x800 at 60 Hz). Set before the 1440p preset, so it keeps only its no-MSAA /
    // upload / texture-load parts. Each setting only if left at default.
    if (REXCVAR_GET(wwe2k14_preset_deck)) {
      for (auto [name, value] : {std::pair{"resolution_scale", "1"},           // 1280x720, letterboxed in 1280x800
                                 std::pair{"wwe2k14_entrance_60fps", "true"},   // 60 fps entrances
                                 std::pair{"gpu_native", "2"},                  // the native renderer (RADV: interlock)
                                 std::pair{"gpu_fast_clear", "true"},
                                 std::pair{"gpu_vblank_precise_timer_spin_us", "250"}}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
      REXLOG_INFO("[wwe2k14][preset_deck] Steam Deck preset (groundwork): resolution_scale={}, entrance_60fps={}, "
                  "gpu_native={}, fast clears {}, vblank spin {} us",
                  rex::cvar::GetFlagByName("resolution_scale"), rex::cvar::GetFlagByName("wwe2k14_entrance_60fps"),
                  rex::cvar::GetFlagByName("gpu_native"), rex::cvar::GetFlagByName("gpu_fast_clear"),
                  rex::cvar::GetFlagByName("gpu_vblank_precise_timer_spin_us"));
    }

    // Session 7c: --wwe2k14_preset_4k60 = the 1440p60 preset at resolution_scale 3 (set first, so the preset below
    // leaves it alone). Session 7c runs 168+: entrances at 60 fps on both routes at 4K.
    if (REXCVAR_GET(wwe2k14_preset_4k60) &&
        rex::cvar::GetFlagSource("resolution_scale") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("resolution_scale", "3");
    }

    // Session 7b (2026-10-08, unattended; docs/session7b_1440p60_notes.md): 1440p with 60 fps entrances. Rendering
    // at 2x without the game's 4x MSAA costs the GPU less than the MSAA did (run 136); at 60 fps the limit was ~1,000
    // pipeline-draining re-uploads of unchanged data per frame, which gpu_upload_skip_unchanged removes (hash per
    // page; verify run 152: 0 mismatches; also an S7 default since run 159, kept here so the preset works with
    // --wwe2k14_s7_defaults=false). Runs 153-156: entrances at 60 fps and real-time speed on both routes. On by
    // default since Session 8 (Jarrod checked edges, crowd, motion, menus; trons, outfits, music, sound sync and smoke
    // earlier); --wwe2k14_preset_1440p60=false restores the original look.
    // Each setting only if left at default, so any of them can still be overridden.
    if (REXCVAR_GET(wwe2k14_preset_1440p60) || REXCVAR_GET(wwe2k14_preset_4k60)) {
      for (auto [name, value] : {std::pair{"wwe2k14_force_no_msaa", "true"},       // 1 tile instead of 4 strips
                                 std::pair{"resolution_scale", "2"},                // 2560x1440 internal
                                 std::pair{"wwe2k14_entrance_60fps", "true"},       // keep entrances at 60 fps
                                 std::pair{"gpu_upload_skip_unchanged", "true"},    // UPSKIP
                                 std::pair{"vulkan_texture_load_direct", "true"}}) {  // TEXDIRECT
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
    }

    // Session 21-22 GPU defaults (on since 2026-10-15, Jarrod's decision after his flicker check with the 165 bundle:
    // "flicker and speed looked completely fine, no tearing"). Each measured two ways (in-run A/B + replay A/B) with
    // frame hashes identical to the default renderer (docs/research/perf_s21.md, perf_s22.md):
    //   gpu_skip_noop_draws        -0.3..-0.4 ms GPU per frame (the 960 invisible point draws)
    //   vulkan_upload_hoist=2      -0.55..-0.78 ms (uploads copied at the start of the submission)
    //   present_guest_frames_once  one present per game frame (no repeats)
    //   vulkan_texture_load_eager  -0.05..-0.26 ms in entrances (textures loaded right after their resolve)
    //   vulkan_texture_load_direct_depth  -0.1..-0.97 ms (the depth texture in one dispatch; bit-exact)
    // --wwe2k14_s22_defaults=false turns all of them off; each can also be turned off alone. Only if left at default.
    if (REXCVAR_GET(wwe2k14_s22_defaults)) {
      for (auto [name, value] : {std::pair{"gpu_skip_noop_draws", "true"},
                                 std::pair{"vulkan_upload_hoist", "2"},
                                 std::pair{"present_guest_frames_once", "true"},
                                 std::pair{"vulkan_texture_load_eager", "true"},
                                 std::pair{"vulkan_texture_load_direct_depth", "true"}}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
    }

    // Session 23 GPU defaults (on since 2026-10-15, Jarrod's decision after SESSION23_SUMMARY.md): repeat draws. A draw
    // that changes only vertex constants against the one before it (the crowd's ~3,600 sprites per frame in Bryan's
    // heavy shot) is recorded cheaply, and the two crowd shaders' runs of them become instanced draws. Measured two
    // ways (docs/research/perf_s23.md): CP -0.2..-0.9 ms per frame (-2.4 in the heavy shot), GPU -0.15..-0.47
    // (-1.1 in the heavy shot); 165 holds on the whole profile route; frame hashes identical. The merge setting and
    // its shader list are latched when the GPU starts. --wwe2k14_s23_defaults=false turns them off; each can also be
    // turned off alone. Only if left at default.
    if (REXCVAR_GET(wwe2k14_s23_defaults)) {
      for (auto [name, value] :
           {std::pair{"vulkan_repeat_draw_fast", "true"}, std::pair{"vulkan_repeat_draw_merge", "true"},
            std::pair{"vulkan_repeat_draw_merge_shaders", "D5E2FCC9F76BC40C,C1028A64EFCF8229"}}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
    }

    // Session 22 item 1 (unattended; SESSION22_PLAN.md): a 165 Hz setup in one flag, default off (new defaults wait for
    // Jarrod's flicker check). Session 21's winners (docs/research/perf_s21.md, both A/B methods): no-op point draws
    // skipped (-0.3..-0.4 ms GPU per frame), uploads hoisted to the start of the submission (-0.55..-0.78 ms), one
    // present per game frame. The match steps at 165, entrances at 144 (Bryan's entrance is ~0.8-1.0 ms over the 165
    // budget), the guest vblank follows the step; mailbox instead of immediate (every frame shown at 165, no tearing).
    // Later Session 22 winners join here. Each setting only if left at default.
    if (REXCVAR_GET(wwe2k14_preset_165)) {
      for (auto [name, value] : {std::pair{"gpu_skip_noop_draws", "true"},
                                 std::pair{"vulkan_upload_hoist", "2"},
                                 std::pair{"present_guest_frames_once", "true"},
                                 // Session 22 item 3: textures loaded right after the resolve that made them.
                                 std::pair{"vulkan_texture_load_eager", "true"},
                                 // Session 22 item 6: the depth texture loaded in one dispatch (bit-exact).
                                 std::pair{"vulkan_texture_load_direct_depth", "true"},
                                 std::pair{"wwe2k14_step_hz", "165"},
                                 std::pair{"wwe2k14_step_hz_entrance", "144"},
                                 std::pair{"wwe2k14_step_vblank", "true"},
                                 std::pair{"vulkan_allow_present_mode_immediate", "false"}}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
    }
    // Session 31: vulkan_device = the dedicated GPU (or --wwe2k14_gpu), if not set (after the plugin registered it).
    if (REXCVAR_GET(wwe2k14_gpu_backend) != "d3d12") wwe2k14::gpu_pick::Choose();
    wwe2k14::light::ConfigureFallback();  // Session 37: the card's render path in the log (+ --wwe2k14_fbo_fallback_fix)
    wwe2k14::low_power::Configure();      // Session 37: --wwe2k14_low_power (default off)
    wwe2k14::SlowFrameOnPreSetup();  // Session 8 task 12: nothing unless --wwe2k14_slowframe_ms > 0
    wwe2k14::dlaa::Configure();      // Session 18: DLAA phase 1 (all off unless a --wwe2k14_dlaa_* flag is set)
    wwe2k14::gfx_menu::ConfigureTextureFilter();  // Session 33: --wwe2k14_texture_filter (0 = unchanged)
    wwe2k14::ultrawide::Configure();  // Session 33: --wwe2k14_aspect (16:9 = unchanged)
    // Session 33: photo mode is built on the CineCam free camera.
    if (REXCVAR_GET(wwe2k14_photo_mode) && rex::cvar::GetFlagSource("wwe2k14_cinecam") == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName("wwe2k14_cinecam", "true");
    wwe2k14::gfx_menu::CaptureRunning();  // Session 28: what this run does, for the in-game GRAPHICS menu
  }

  void OnPostInitLogging() override {
    wwe2k14::InstallCrashHandlers();
    wwe2k14::ApplyBackgroundFullSpeed();  // Session 32: test lanes behind other windows at full speed
    // Session 8 task 0: pin this run's log number now, inside run_and_triage's launch window (frame_stats.h).
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
    // Session 37: the process's main (UI) thread had no name: thread_busy.ps1 showed it as "tid N" at ~99% of a core
    // in a match (run 2622; start address wWinMainCRTStartup, tools\perf\thread_starts.ps1). Named for the profilers.
    SetThreadDescription(GetCurrentThread(), L"wwe2k14 UI");
    REXLOG_INFO("[wwe2k14] UI thread {} named \"wwe2k14 UI\"", GetCurrentThreadId());
#else
    const unsigned long pid = 0;
#endif
    REXLOG_INFO("[wwe2k14] run log number: {:03d} (pid {})", std::max(wwe2k14::CurrentLogNumber(), 0), pid);
#ifdef WWE2K14_VERSION
    REXLOG_INFO("[wwe2k14] version {}", WWE2K14_VERSION);  // Session 25: VERSION (CMakeLists.txt)
#endif
  }

  // Record the effective settings in every run's log.
  void OnPostSetup() override {
    REXLOG_INFO("[wwe2k14] game_data_root: {} ({})", game_data_root().string(),
                std::filesystem::is_regular_file(game_data_root()) ? "disc image" : "folder");
    REXLOG_INFO("[wwe2k14] user_data_root: {}", user_data_root().string());  // Session 53: the save folder in use
    wwe2k14::keybinds_save::Install(user_data_root());  // Session 53 item 2.6: keyboard binds kept between starts
    for (const char* name : {"gpu_plugin", "wwe2k14_gpu_backend", "readback_resolve",
                             "gpu_allow_invalid_fetch_constants", "vsync",
                             "wwe2k14_ignore_offset_for_ranged_allocations", "log_max_file_size_mb",
                             "log_max_files"}) {
      REXLOG_INFO("[wwe2k14] {} = '{}' (source: {})", name, rex::cvar::GetFlagByName(name),
                  SourceName(rex::cvar::GetFlagSource(name)));
    }
    // Every non-default setting, so experiment flags are always on record.
    for (const std::string& name : rex::cvar::ListModifiedFlags()) {
      REXLOG_INFO("[wwe2k14] modified: {} = '{}' (source: {})", name,
                  rex::cvar::GetFlagByName(name), SourceName(rex::cvar::GetFlagSource(name)));
    }

    if (REXCVAR_GET(wwe2k14_mount_cache)) MountCachePartition();
    wwe2k14::mods::Install(runtime()->file_system(), game_data_root());  // no-op unless --wwe2k14_mods (Session 17)
    wwe2k14::dlc::Install(runtime()->file_system(), game_data_root());   // no-op unless --wwe2k14_dlc (Session 32)
    wwe2k14::StartTimelineIfRequested();
    wwe2k14::StartThreadPlacement();  // logs the CPU topology; watcher only with --wwe2k14_thread_placement
    wwe2k14::StartFlightRecorder();   // no-op unless --wwe2k14_flight_recorder (Session 8 task 11)

    // Input scripts can end the run the same way Alt+F4 does.
    wwe2k14::g_request_quit = [this] {
      app_context().CallInUIThreadDeferred([this] {
        // RequestClose closes without asking OnWindowCloseRequested (SDL: PerformClose), so write it here too.
        wwe2k14::perf_report::Write();  // no-op unless --wwe2k14_perf_report (Session 20)
        if (window()) window()->RequestClose();
      });
    };
    wwe2k14::replay::g_replay.Init();  // no-op unless --wwe2k14_replay_* (Session 16)
    if (!wwe2k14::replay::g_replay.StopReason().empty()) {
      // Session 32: a replay whose DLC packs are not on: say so (a box when the launcher asks for one; every other run
      // only logs it, so an unattended run never waits on a click: review), close.
      if (REXCVAR_GET(wwe2k14_replay_dlc_box)) {
        const std::string& why = wwe2k14::replay::g_replay.StopReason();
        MessageBoxW(nullptr, std::wstring(why.begin(), why.end()).c_str(), L"WWE 2K14: replay", MB_OK | MB_ICONWARNING);
      }
      wwe2k14::g_request_quit();
    }
    wwe2k14::reshoot::Install();       // no-op unless --wwe2k14_reshoot (Session 16)
    wwe2k14::matte::Install();         // no-op unless --wwe2k14_matte (Session 16)
    wwe2k14::runin::Install();         // no-op unless --wwe2k14_runin (Session 16)
    wwe2k14::director::Install();
    wwe2k14::tron_export::Install();  // no-op unless --wwe2k14_tron_export      // Session 17: --wwe2k14_director (after runin: its handler runs run-ins)
    wwe2k14::audio_capture::Install();  // no-op unless --wwe2k14_audio_capture (Session 18)
    wwe2k14::g_input_script.Init();
    wwe2k14::live::Install();  // no-op unless --wwe2k14_live (Session 29)
  }

  // Session 20 item 9: a window close (Alt+F4) asks here first, before ReXGlue's hard exit in OnClosing: the last point
  // to write the playtester report. An input script's quit (RequestClose) skips this; g_request_quit writes it.
  bool OnWindowCloseRequested() override {
    wwe2k14::perf_report::Write();  // no-op unless --wwe2k14_perf_report
    return true;
  }

  // Only on ReXGlue's OnDestroy path; the usual close hard-exits (std::_Exit in ReXApp::OnClosing) before this.
  void OnShutdown() override {
    wwe2k14::StopThreadPlacement();
    wwe2k14::inspector::Shutdown();  // no-op unless --wwe2k14_inspector
    wwe2k14::cinecam::Shutdown();    // no-op unless --wwe2k14_cinecam
    wwe2k14::check::Shutdown();      // Session 29: no-op outside harness runs
    wwe2k14::bookmark::Shutdown();   // Session 29
    wwe2k14::audio_capture::Shutdown();  // Session 18
  }

  // Session 8 task 15: ImGui live inspector (src/inspector.h). Returns at once unless --wwe2k14_inspector=true.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    ApplyWindowIcon();  // Session 17: game_logo.png, embedded by the build (no-op without it)
    wwe2k14::inspector::Install(drawer, PPCImageConfig.code_base, PPCImageConfig.code_size, PPCImageConfig.image_base,
                                PPCImageConfig.image_size,
                                [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); });
    wwe2k14::runin::InstallBind();  // Session 16: F9 = run-in (no-op unless --wwe2k14_runin)
    wwe2k14::cinecam::Install(window(), drawer);  // Session 13 CineCam free camera; no-op unless --wwe2k14_cinecam
    wwe2k14::photo::Install();  // Session 33 photo mode; no-op unless --wwe2k14_photo_mode
    wwe2k14::wm30::Install();   // Session 34; no-op unless --wwe2k14_wm30
    // Session 37: a failed FSR is turned off with a notice (no-op unless --wwe2k14_fsr).
    wwe2k14::upscale_guard::Install(
        drawer, [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); });
    // Session 29: `check TEXT` banner + F1 / F2 / F5, only in harness runs (an input script or the live channel).
    const bool harness = !REXCVAR_GET(wwe2k14_input_script).empty() || !REXCVAR_GET(wwe2k14_live).empty();
    wwe2k14::check::Install(
        drawer, [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); }, harness,
        window() ? window()->GetNativeWindowHandle() : nullptr);
    wwe2k14::bookmark::Install(harness);  // Session 29: F11 = "that, just now" bundle (harness runs only)
    wwe2k14::screen_text::Enable(harness);  // Session 30: text the game fetched, for scripts / live / bookmarks
  }

  // Image is loaded and functions registered, guest code not started yet.
  void OnPreLaunchModule() override {
    wwe2k14::ScanForUnregisteredFunctionPointers(runtime(), PPCImageConfig, 2000);
    DumpRequestedWords();
    wwe2k14::DumpGuestImage(runtime(), PPCImageConfig, REXCVAR_GET(wwe2k14_dump_image));  // empty = off
  }

 private:
  // Session 17: the window's title-bar / taskbar icon from icon resource 1 (CMakeLists.txt embeds it from
  // game_logo.png when present). ReXGlue's window doesn't apply one itself, so every top-level window of this process
  // gets it (WM_SETICON + its class icon).
  static void ApplyWindowIcon() {
#if defined(_WIN32) && defined(WWE2K14_HAS_ICON)
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HICON icon_big = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                              GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON icon_small = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!icon_big) return;
    struct Icons {
      HICON big, sm;
    } icons{icon_big, icon_small ? icon_small : icon_big};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
          DWORD pid = 0;
          GetWindowThreadProcessId(h, &pid);
          if (pid != GetCurrentProcessId()) return TRUE;
          const auto* ic = reinterpret_cast<const Icons*>(lp);
          SendMessageW(h, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(ic->big));
          SendMessageW(h, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ic->sm));
          SetClassLongPtrW(h, GCLP_HICON, reinterpret_cast<LONG_PTR>(ic->big));
          SetClassLongPtrW(h, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(ic->sm));
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&icons));
#endif
  }

  // Own device path: \Device\Harddisk0\Cache0 is already claimed by ReXGlue's NullDevice.
  void MountCachePartition() {
    constexpr std::string_view kMountPath = "\\Device\\WWE2K14Cache";
    const auto host_path = user_data_root() / "cache_partition";
    std::error_code ec;
    std::filesystem::create_directories(host_path, ec);
    auto* file_system = runtime()->file_system();
    auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMountPath, host_path, false);
    if (!device->Initialize() || !file_system->RegisterDevice(std::move(device)) ||
        !file_system->RegisterSymbolicLink("cache:", kMountPath)) {
      REXLOG_WARN("[wwe2k14] failed to mount cache: at {}", host_path.string());
      return;
    }
    REXLOG_INFO("[wwe2k14] cache: mounted at {}", host_path.string());
  }

  void DumpRequestedWords() {
    std::stringstream requests(REXCVAR_GET(wwe2k14_dump_words));
    std::string item;
    while (std::getline(requests, item, ',')) {
      auto colon = item.find(':');
      uint32_t addr = uint32_t(std::strtoul(item.substr(0, colon).c_str(), nullptr, 0));
      uint32_t count = colon == std::string::npos ? 1 : uint32_t(std::strtoul(item.substr(colon + 1).c_str(), nullptr, 0));
      for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = runtime()->memory()->TranslateVirtual<const uint8_t*>(addr + 4 * i);
        uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        REXLOG_INFO("[wwe2k14][dump] 0x{:08X} [{}] = 0x{:08X}", addr + 4 * i, i, v);
      }
    }
  }

  static std::string_view SourceName(rex::cvar::Source source) {
    switch (source) {
      case rex::cvar::Source::kDefault: return "default";
      case rex::cvar::Source::kConfig: return "config file";
      case rex::cvar::Source::kEnvironment: return "environment";
      case rex::cvar::Source::kCommandLine: return "command line";
      case rex::cvar::Source::kRuntime: return "runtime/code";
    }
    return "?";
  }

};
