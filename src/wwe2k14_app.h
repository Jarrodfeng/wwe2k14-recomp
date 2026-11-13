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

#include "guest_gpu_only.h"  // lane 241 (3b)
#include "blackbox.h"  // Session 73: logs/startup.txt (start-up steps, crashes, low memory)
#include "android_memory.h"  // Session 73: Android's memory warnings (GameActivity.onTrimMemory)
#include "crash_handler.h"
#include "gpu_timeline.h"
#include "android_surface_events.h"  // lane 235: SDL app-state events to the swapchain
#include "android_pace.h"  // Session 93: --wwe2k14_android_pace_gate / _thermal, present pacing's start lines
#include "android_dev_flags.h"  // S101 lane 127: <files>/dev_flags.txt (promotion switches) in a +dev APK
#include "low_ram.h"  // --wwe2k14_low_ram (Session 94)
#include "d3d_owned.h"  // --wwe2k14_d3d_owned (Session 87, the D3D-layer stack in one switch, default off)
#include "pacing_hooks.h"
#include "tiling_hooks.h"
#include "kernel_overrides.h"
#include "missing_function_scan.h"
#include "thread_placement.h"
#include "power_throttle.h"  // --wwe2k14_eco_qos / --wwe2k14_pack_cores (Session 95, default off)
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
#include "crowd_cards.h"    // --wwe2k14_crowd_card_distance (Session 100, default 1 = the game's own)
#include "crowd_wide.h"     // lane 176: --wwe2k14_crowd_fill_wide (auto = the stands crowd's screen test follows the displayed aspect; config/crowd_wide_hooks.toml)
#include "flow_probe.h"     // --wwe2k14_flow_probe (Session 14, Rumble diagnosis, default off)
#include "reshoot.h"        // --wwe2k14_reshoot (Session 16 item 2, default off)
#include "runin.h"          // --wwe2k14_runin (Session 16 item 9 experiment, default off)
#include "director.h"       // --wwe2k14_director (Session 17 Track 3, default off)
#include "tron_shim.h"
#include "lighting_mod.h"  // --wwe2k14_mods_lighting (Session 98 step 13: arena / entrance light colours from <mods>\lighting, default off)
#include "scene_live/arena_hooks.h"  // S101 lane28: the arena's evaluated light sets for the live scene record (post-hook on sub_827F8730; free unless a scene sink is attached)
#include "renderer_scene/skeleton_want.h"  // lane 207
#include "scene_live/wrestler_contrib.h"  // S101 lane 1: RegisterWrestlers (lane 29's class), wired in OnPostSetup
#include "scene_live/arena_world_contrib.h"  // lane 130: RegisterArenaWorlds (the arena's object worlds + stage clock), wired in OnPostSetup
#include "scene_live/sphere_contrib.h"  // lane 242: RegisterSpheres (H3, the wrestlers' sphere occluders; the caster record's switches), wired in OnPostSetup
#include "scene_live/caster_contrib.h"  // lane 202: RegisterCasters (the caster passes' per-entry record, Caster D3 E0-E2; verify / census arm only), wired in OnPostSetup
#include "scene_live/ring_rows_contrib.h"  // lane 194: RegisterRingRows (the ring's per-call node rows, Ring D3 R2; verify / census arm only), wired in OnPostSetup
#include "scene_live/light_state_contrib.h"  // S101 lane 109: RegisterLightStates (the per-draw light state, LS1), wired in OnPostSetup
#include "scene_live/plane_contrib.h"  // lane 139: RegisterPlane (the plane shadow chain's constants), wired in OnPostSetup
#include "scene_live/circle_contrib.h"  // lane 136: RegisterCircles (the circle blob shadows' constants), wired in OnPostSetup
#include "logo_import.h"    // --wwe2k14_logo_import (Session 18, logo importer v0, default off)
#include "audio_capture.h"  // --wwe2k14_audio_capture (Session 18, sound for re-shoots, default off)
#include "precompile.h"     // --wwe2k14_precompile (Session 70: the launcher's precompile, default off)
#include "native_frame.h"   // --wwe2k14_native_frame (Session 79: the HD 520 arc's switch, default off)
#include "tron_export.h"    // --wwe2k14_tron_export (Session 17 launcher previews, default off)      // MP4 / MOV / MKV titantrons via Media Foundation (Session 17 item 1B; needs --wwe2k14_mods)
#include "motion_fix.h"     // --wwe2k14_fix_secondary_motion (Session 27.5: Cena's title at 60 fps, default off)
#include "gfx_menu.h"       // GRAPHICS menu in the Options (Session 28)
#include "dlc.h"            // --wwe2k14_dlc: the player's DLC packs (Session 32, default off)
#include "background_speed.h"  // --wwe2k14_background_full_speed (Session 32, default off; run_list turns it on)
#include "display_mode.h"      // --wwe2k14_display_720p (Session 95, default off): 1280x720 desktop while playing
#include "live_control.h"   // --wwe2k14_live (Session 29: live control channel, default off)
#include "check_prompt.h"   // input-script `check TEXT` (Session 29: on-screen question, harness runs only)
#include "perf_overlay.h"   // --wwe2k14_perf_overlay (Session 80: frame rate, frame time, temperature, a graph)
#include "vulkan_layers.h"  // Session 36 (GitHub issue 1): Overwolf's Vulkan layers kept out (they crashed the game)
#include "gpu_pick.h"       // Session 31: the dedicated GPU by default, --wwe2k14_gpu=NAME (the launcher's chooser)
#include "intel_exact_mul.h"  // Session 76: on Intel GPUs NATIVE keeps the 360's multiply rule (--wwe2k14_intel_exact_mul)
#include "light_renderer.h" // Session 37: --wwe2k14_light_renderer (weak GPUs), --wwe2k14_fbo_fallback_fix (default off)
#include "android_touch.h"  // Session 71 (Android): the touch controls' virtual pad (TouchOverlay.java)
#include "job_dispatch_fix.h"  // Session 71: --wwe2k14_job_reset_first (the entrance freeze on 4-thread CPUs, default on)
#include "photo_mode.h"     // --wwe2k14_photo_mode (Session 33 item 4, default off)
#include "ultrawide.h"      // --wwe2k14_aspect (Session 33 item 6, default 16:9 = off)
#include "wm30_jump.h"      // --wwe2k14_wm30 (Session 34: jump into a 30 Years of WrestleMania match, default off)
#include "scene_director.h"  // S101 lane 32: !scene (any scene on demand through the game's scene machine; live channel, default off)
#include "caw_live.h"        // S101 lane 118: !caw (the Create a Superstar editor's drawn state, read from the frame's text runs; live channel, off until used)
#include "wm30_step.h"       // S101 lane 121: !wm30 step K (the match script driven into a step; the faithful 30 Years moment)
#include "situation_director.h"  // S101 lane 161: !director passive / momentum / stock (the situation director's first verbs; live channel + input script, default off)
#include "wm30_fire.h"       // S101 lane 108: !wm30 fire BANK (a 30 Years scene fired through the game's own Lua call 1003; live channel, default off)
#include "upscale_guard.h"  // Session 37: a failed FSR turns itself off with a notice (with --wwe2k14_fsr)
#include "direct_start.h"   // Session 53: a release started without the launcher (hand-off, settings file, message)
#include "keybinds_save.h"  // Session 53: the keyboard binds saved in the save folder (--wwe2k14_save_keybinds)
#include "keyboard_typing.h"  // Session 55: --wwe2k14_keyboard_typing (the PC keyboard types into the game's keyboard)
#include "keyboard_player.h"  // Session 76: --wwe2k14_keyboard_own_player (the keyboard is a player of its own)
#include "focus_widget.h"  // Session 91: which input widget has the input, and its cursor (the navigator's game_where)
#include "mouse_menu.h"  // Session 67: --wwe2k14_mouse_menus (the mouse points, clicks and scrolls in the list menus)
#include "create_readback.h"  // Session 57: --wwe2k14_readback_sequences (exact readback in the create modes, default on)
#include "load_pipeline_wait.h"  // Session 57: --wwe2k14_load_pipeline_wait (no skipped draws during in-game loads)
#include "post_swap.h"  // Session 72: our post-effect shaders (gpu_native_shader_swap)
#include "scene_swap.h"  // Session 80: our own scene material shaders (--wwe2k14_scene_shaders, off)
#include "scene_vs_swap.h"  // Session 87 stage 4: our own arena vertex shaders (--wwe2k14_vs_swap, off)
#include "session_log.h"  // Session 58: --wwe2k14_session_log (scene / match lines and snapshots for problem reports)
#include "surface_roles_hook.h"  // S101 names A1b: which function created each render target (docs\renderer\NAMES.md)
#include "replay_watch.h"  // Session 63: log a runaway render-command replay; S73 b78: end it (default on Android)
#include "xdet_check.h"  // Session 77 (P7): --wwe2k14_xdet_check, the tester determinism check's profile
#include "diag.h"        // Session 95: --wwe2k14_diag, the diagnostic run (also reached through input_script.h)
#include "native_skin.h"   // Session 64: the game's CPU vertex-skinning loop sub_82508738 natively (S66: every platform, via native_fn.h)
#include "nf/all.h"        // Session 66 (F2): the native function factory's functions (off until verified)
#include "crowd_skin_report.h"  // Session 100 strip-360 E3: the crowd's 1-bone / 4-influence skinning report their output to the reader
#include "vb_unlock_report.h"   // Session 100 strip-360 E3: the game's vertex-buffer unlock reports the buffer to the reader
#include "shader_list.h"  // Session 61: --wwe2k14_shader_list (the shipped shader list into the shader storage)
#include "renderer_adapter/overlay_drawer.h"  // Session 100 settings parity S11
#include "renderer_adapter/d3_latch_hook.h"  // lane 301: g_d3_latch_hook

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
// Session 95: the CPU / upload power set, one switch, OFF until its check passes (each part is its own SDK flag and still
// works alone; gpu_hot_pages_frames left the set in the review, `became hot` 0 in every log: vulkan_upload_hoist_merge, vulkan_hoist_tight_barrier, kernel_cs_spin_pause,
// kernel_timestamp_tick_ms, present_ui_idle_wait_ms, wwe2k14_low_power_spin_us / _sleep_us, wwe2k14_load_turbo_vblank_hz).
REXCVAR_DEFINE_BOOL(wwe2k14_scene_light_states, false, "wwe2k14",
                    "Session 101 lane 109: the per-draw light state (the render camera record R read at every model call) and the arena worlds (the palette matrix of every model call + the stage clock, lane 130) join the scene records. Needs --wwe2k14_scene_records. Off by default: it costs about 0.5 us per model call and nothing reads the tables live yet; the milestone capture turns it on");
REXCVAR_DEFINE_BOOL(wwe2k14_scene_circles, false, "wwe2k14",   // lane 136
                    "Session 101 lane 136: the circle blob shadows' constants (c0 ShadowParam, ViewProj, the 21 body points) and their disc mesh join the scene records, read at the game's own upload (sub_82812E90). "
                    "Needs --wwe2k14_scene_records. Also switched on by --wwe2k14_rdr_circles_ours. Off by default: three compares per constant upload, a few reads on the four a frame that match");
REXCVAR_DEFINE_BOOL(wwe2k14_scene_plane, false, "wwe2k14",   // lane 139
                    "Session 101 lane 139: the plane shadow chain's vertex constants (the mask's PlaneInv, camera, ShadowDistance, PlanePos, EyePos, palette; the blur's matrix) join the scene records, read at the game's own "
                    "uploads (sub_82BF22C8 / sub_827ECD10) and snapshot at the draws. Needs --wwe2k14_scene_records. Also switched on by --wwe2k14_rdr_plane_shadow_ours. Off by default: a range compare per constant upload");
REXCVAR_DEFINE_BOOL(wwe2k14_s95_lean, false, "wwe2k14",
                    "Session 95: turn on the CPU / upload power set (merged hoisted copies, tight "
                    "hoist barriers, critical section PAUSE spin, 4 ms kernel timer, 8 ms UI idle wait, no render-thread spin, "
                    "load turbo at 4 kHz). Each flag below still wins when set on the command line.");
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

// Session 95 (item F6, gpu_d3d_decouple phase 2): 120 presented pictures a second at logic 60, in place of 120 whole game steps a second.
REXCVAR_DEFINE_BOOL(wwe2k14_decouple_120, false, "wwe2k14",
                    "Session 95 (EXPERIMENTAL, default off; read at start): the 120FPS mode as two pictures per game step - sets gpu_d3d_decouple=9 and "
                    "gpu_d3d_decouple_word=0 (the owned passes render 2 views, the second a half tick on; SDK decouple_blend.cpp) when those are left at "
                    "default. The game steps at its own 60 (no wwe2k14_step_hz / step_vblank); the launcher's mapping (gfx_settings.h, "
                    "mobile_settings.h) pushes the same pair for FRAME RATE 120 when WWE2K14_DECOUPLE_120=1 is in its environment"); 

// Session 6: one switch for all of this session's command-processor optimisation defaults (see OnPreSetup).
REXCVAR_DEFINE_BOOL(wwe2k14_s6_defaults, true, "wwe2k14",
                    "Apply the Session 6 GPU command-processor optimisation defaults (false = Session 5 behaviour)");

// Session 101 (Jarrod 2026-11-13 02:4x stretch interview, DECISIONS): where a class passed its Check K rows, its promotion goes DEFAULT ON on the
// session branch only (unreleased; a release decides it again). PC and phone (the phone since 2026-11-13 11:1x, Jarrod: the night APK must show tonight's work).
// Bundle 2 DEFAULT ON since 2026-11-13 15:2x (Jarrod: "I want everything on"; DECISIONS), PC and phone (the RC APK playtest is its first phone run).
REXCVAR_DEFINE_BOOL(wwe2k14_s101_promotions2, true, "wwe2k14",
                    "Session 101, bundle 2 (default on, PC and phone): --wwe2k14_rdr_readback_async + _defer (replay thumbnail, run 7164), "
                    "--wwe2k14_owner_boot_submit_ours + --wwe2k14_owner_guest_gpu_only (no emulated 360 GPU, 7163), --wwe2k14_owner_vblank_ours "
                    "(our own vblank, 7184 / 7185 / 7188). The D3 skips (caster / ring / plane / crowd member / arena) are proven but off for this "
                    "release (Surface: they cost the render thread more than they save). Each only if left at default; false = all of them off.");
REXCVAR_DEFINE_BOOL(wwe2k14_s101_promotions, true, "wwe2k14",
                    "Session 101 branch: start the PC in owner mode with every renderer promotion that passed Check K (the J+ set: crowd, cards, floor, "
                    "arena, ring, wrestlers, props, flashes, particles, UI, UI text / quads, casters, shadows, rim + rim chain, screens, post, movie, small "
                    "passes, circles; scene records + light states, which they read). Each only if left at default; false = the old PC defaults in one flag. "
                    "Not the plane shadow (it needs casters off), not the cuts (arena / ring / caster D3, bake skip, card thin, copy alias). PC and phone");

#if defined(__ANDROID__)
REXCVAR_DEFINE_INT32(wwe2k14_android_pin_cpu, -1, "wwe2k14",
                     "Session 63 (Android, diagnosis): run the game on this CPU core only (-1 = off)");
#endif

// Session 83 (GitHub #61 / #80): default on. MATCH HIGHLIGHTS needs it: sub_8260AC58 checks cache:\ for 157 MB free
// (2400 x 64 KB), creates cache:\Spectacular.rps at that size and only then sets the replay flag 0x83767CF8; without
// cache: the post-match menu's MATCH HIGHLIGHTS was off in every build. The game also creates CAR.cac / EditEx.cac.
REXCVAR_DEFINE_BOOL(wwe2k14_present_mailbox, false, "wwe2k14",
                    "Session 83 (GitHub #79): with vsync on, present MAILBOX instead of the SDK's IMMEDIATE (no tearing "
                    "on screens without G-SYNC / FreeSync). Off by default: it made entrances choppier in the v0.2.17 "
                    "release check");
REXCVAR_DEFINE_BOOL(wwe2k14_mount_cache, true, "wwe2k14",
                    "Mount <user data>/cache_partition as the Xbox 360 HDD cache partition (cache:); MATCH HIGHLIGHTS "
                    "needs it (the game keeps its replay file there)");

// Session 95 (flat guest memory): the generated code's offset (the codegen template) and the SDK header's must agree, or our
// hooks and the recompiled code read the 0xE0000000 range 4 KB apart. An SDK update without a fresh codegen stops here.
static_assert(REX_PHYS_HOST_OFFSET(0xE0000000u) == ::rex::memory::detail::PhysicalHostOffset(0xE0000000u),
              "generated code and rex/system/xmemory.h disagree: run codegen after the SDK build");

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
    // Session 77 (P7): the tester check's profile first (over every other source; no wwe2k14.toml).
    wwe2k14::xdet_check::ConfigurePaths(paths);
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
    wwe2k14::blackbox::Step("set-up: settings");
    wwe2k14::android_dev_flags::Apply();  // S101 lane 127: a +dev APK reads <files>/dev_flags.txt (the --wwe2k14_rdr_*_ours / --wwe2k14_scene_* switches); first, before anything reads them
    // S101: the Check K promotions default on (branch only; see wwe2k14_s101_promotions). Before the owner test below, which reads wwe2k14_rdr_owner.
    // Phone too since 2026-11-13 11:1x (Jarrod: "How am I meant to test the work of tonight without an apk that actually switches it on?"); a +dev APK's
    // dev_flags.txt (applied just above) still wins per switch, and --wwe2k14_s101_promotions=false there gives the old phone defaults.
    if (REXCVAR_GET(wwe2k14_s101_promotions)) {
      int applied = 0;
      for (const char* name : {"wwe2k14_rdr_owner", "wwe2k14_scene_records", "wwe2k14_scene_light_states",
                               "wwe2k14_rdr_crowd_ours", "wwe2k14_rdr_crowd_floor", "wwe2k14_rdr_crowd_skip_skin", "wwe2k14_rdr_crowd_cards",
                               "wwe2k14_rdr_crowd_props_ours", "wwe2k14_rdr_arena_ours", "wwe2k14_rdr_arena_thin", "wwe2k14_rdr_wrestlers_ours",
                               "wwe2k14_rdr_wrestlers_thin", "wwe2k14_rdr_ring_ours", "wwe2k14_rdr_ring_thin", "wwe2k14_rdr_flash_ours",
                               "wwe2k14_rdr_props_ours", "wwe2k14_rdr_particles_ours", "wwe2k14_rdr_ui_ours", "wwe2k14_rdr_ui_text_ours",
                               "wwe2k14_rdr_shadow_casters_ours", "wwe2k14_rdr_shadows_ours", "wwe2k14_rdr_rim_ours", "wwe2k14_rdr_screens_ours",
                               "wwe2k14_rdr_post_ours", "wwe2k14_rdr_movie_ours", "wwe2k14_rdr_ui_quads_ours", "wwe2k14_rdr_small_passes_ours",
                               "wwe2k14_rdr_circles_ours", "wwe2k14_rdr_rim_chain_ours", "wwe2k14_rdr_caw_ours", "wwe2k14_rdr_plane_shadow_ours"}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) { rex::cvar::SetFlagByName(name, "true"); ++applied; }
      }
      REXLOG_INFO("[wwe2k14] S101 promotions default on: {} switches applied (the rest set explicitly); --wwe2k14_s101_promotions=false for the old defaults", applied);
    }
    // S101 bundle 2 (DEFAULT ON since 2026-11-13 15:2x, Jarrod: "I want everything on"): the switches proven live that take the game's own work away.
    // Thumbnail deferral (run 7164), caster D3 skip arm (7168), the boot answered by us + no emulated 360 GPU (7163), our own vblank (7184 / 7188).
    // Phone too: lane 256 audited / fixed the Android paths; the RC APK playtest is the first phone run. Each switch refuses itself where its
    // preconditions do not hold (owner mode, det modes, the SDK patch), with a log line.
    if (REXCVAR_GET(wwe2k14_s101_promotions2) && REXCVAR_GET(wwe2k14_s101_promotions)) {
      int applied = 0;
      for (const char* name : {"wwe2k14_rdr_readback_async", "wwe2k14_rdr_readback_defer",
                               "wwe2k14_owner_boot_submit_ours", "wwe2k14_owner_guest_gpu_only", "wwe2k14_owner_vblank_ours"}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) { rex::cvar::SetFlagByName(name, "true"); ++applied; }
      }
      // The five D3 skips (caster map / rim --wwe2k14_rdr_caster_d3, ring --wwe2k14_rdr_ring_d3 + _live, caster plane
      // --wwe2k14_rdr_caster_d3_plane, crowd member --wwe2k14_rdr_crowd_member_thin=2, arena --wwe2k14_rdr_arena_d3) are all proven correct
      // live (7168, 7202-7206, 7216) but are NOT in the default for this release (Jarrod 2026-11-13 19:5x, DECISIONS): on the Surface
      // (4-core laptop) they make the game reach 60 while our render thread re-binds nearly a whole frame for every skipped one, so the
      // picture drops to ~28 shown vs ~47-53 with them off (runs 181-193, docs\BENCHMARKS.md). They come back in arc part 2.
      REXLOG_INFO("[wwe2k14] S101 promotions 2 on: {} switches applied (thumbnail deferral, boot ours + no emulated GPU + our vblank; the D3 skips are off for this release); --wwe2k14_s101_promotions2=false for the old path", applied);
    }
    // Session 100 stage B (docs\renderer\LIVE_ADAPTER.md): the new renderer owns the window; the SDK keeps its command processor.
    if (REXCVAR_GET(wwe2k14_rdr_owner)) {
      config.present_detached = true;
      // Lane 241 (LEAVE_THE_COMMAND_STREAM 3b wiring, default off): the SDK builds no provider / command processor / presenter (SDK patch 113). It needs the
      // game's fences, interrupts and flips answered by us (owner timing), the ring submits skipped (owner_no_ring) and the boot submit answered by us
      // (3a); the no-ring queue (gpu_d3d_noring) has no consumer without the processor, so the exe's queue entry is refused while this is on (d3d_noring.h).
      if (REXCVAR_GET(wwe2k14_owner_guest_gpu_only)) {
        if (REXCVAR_GET(wwe2k14_owner_timing) && REXCVAR_GET(wwe2k14_owner_no_ring) && REXCVAR_GET(wwe2k14_owner_boot_submit_ours)) {
          config.guest_gpu_only = true;
          wwe2k14::g_guest_gpu_only.store(true, std::memory_order_relaxed);
          REXLOG_INFO("[owner][gpu-only] guest_gpu_only on: no SDK graphics provider, no command processor, no presenter (MMIO + VSync only)");
        } else {
          REXLOG_WARN("[owner][gpu-only] --wwe2k14_owner_guest_gpu_only REFUSED: it needs --wwe2k14_owner_timing, --wwe2k14_owner_no_ring and --wwe2k14_owner_boot_submit_ours all on (timing {}, no_ring {}, boot_submit_ours {}); the SDK keeps its command processor",
                      REXCVAR_GET(wwe2k14_owner_timing), REXCVAR_GET(wwe2k14_owner_no_ring), REXCVAR_GET(wwe2k14_owner_boot_submit_ours));
        }
      }
      // Lane 260 (LEAVE_THE_COMMAND_STREAM 3e, default off): the guest vblank from OUR clock (the "Owner timing" thread), the SDK starting no VSync thread (SDK patch 114,
      // RuntimeConfig::guest_vblank_ours). Needs guest_gpu_only (the processor-less SDK), no determinism harness (its vblank gating lives in the SDK's VSync loop) and an SDK
      // build that has patch 114 (the exe compiles against the installed headers); otherwise refused with a line and the SDK's VSync thread stays.
      if (REXCVAR_GET(wwe2k14_owner_vblank_ours)) {
        using CfgT = std::remove_reference_t<decltype(config)>;
        const char* refuse = !wwe2k14::GuestGpuOnly()       ? "--wwe2k14_owner_guest_gpu_only is not on (or was refused)"
                             : rex::det::Enabled()          ? "det_frames is on (the determinism harness gates vblanks inside the SDK's VSync loop; not ported)"
                             // S101 16:2x (run 7197 hung after frame 1 on a replay route once bundle 2 went default ON): replay / reshoot turn det_frames on LATER in
                             // this function (replay.h preset, reshoot::PreSetup), so ask for them directly here.
                             : (REXCVAR_GET(wwe2k14_replay_mode) || !REXCVAR_GET(wwe2k14_replay_play).empty() || !REXCVAR_GET(wwe2k14_replay_record).empty() ||
                                !REXCVAR_GET(wwe2k14_reshoot).empty() || rex::cvar::GetFlagByName("det_frames") == "true")
                                 ? "a replay / reshoot / det_frames run is asked for (the determinism harness gates vblanks inside the SDK's VSync loop; not ported)"
                             : !wwe2k14::owner_timing::kSdkHasVblankOurs<CfgT> ? "this SDK build lacks patch 114 (RuntimeConfig::guest_vblank_ours): apply patches\\rexglue-sdk\\full\\114 and rebuild with tools\\build_sdk.ps1"
                                                            : nullptr;
        if (refuse) {
          REXLOG_WARN("[owner][vblank] --wwe2k14_owner_vblank_ours REFUSED: {}; the SDK's VSync thread keeps ticking the vblank", refuse);
        } else {
          wwe2k14::owner_timing::SetVblankOurs(config);
          wwe2k14::owner_timing::g_vblank_ours.store(true, std::memory_order_relaxed);
          REXLOG_INFO("[owner][vblank] vblank_ours on: no SDK VSync thread; the 'Owner timing' thread dispatches the guest vblank (60 Hz vsync on, 1 kHz / --gpu_vblank_unthrottled_hz vsync off)");
        }
      }
      REXCVAR_SET(wwe2k14_rdr_capture, true);
      REXCVAR_SET(wwe2k14_rdr_live, true);
    } else if (REXCVAR_GET(wwe2k14_owner_vblank_ours)) {
      REXLOG_WARN("[owner][vblank] --wwe2k14_owner_vblank_ours ignored: owner mode (--wwe2k14_rdr_owner) is off; the SDK's VSync thread keeps ticking the vblank");
    }
    wwe2k14::rdr_reader::InstallProvenanceAtStart();  // S101 lane17: the writer hook, only when provenance is wanted (before the game starts: frame-0 reads)
    // The old renderer's crowd shortcuts take fans out of the game's draw calls (each group's first member recorded, the rest and the far
    // stands' cards queued to the SDK's command processor, src\scene_crowd.h): the adapter reader never sees them, and the new renderer's
    // upper stands came out empty (S100 run 6855 against v0.3.1). Off whenever the reader runs, before the launcher's settings (those only
    // fill switches still at default).
    if (REXCVAR_GET(wwe2k14_rdr_capture) && !REXCVAR_GET(wwe2k14_rdr_debug_keep_crowd_shortcut)) {
      rex::cvar::SetFlagByName("wwe2k14_scene_crowd", "0");
      rex::cvar::SetFlagByName("wwe2k14_scene_crowd_cards", "false");
    }
    wwe2k14::vulkan_layers::Apply();  // Session 36: before anything starts Vulkan (GitHub issue 1)
#if defined(__ANDROID__)
    wwe2k14::android_driver::Install();  // Session 72: a custom Vulkan driver chosen in the launcher
    wwe2k14::blackbox::Step("set-up: Vulkan driver chosen (src/android_driver.h)");
#endif
    // Session 16 item 1: replay mode preset and a playback file's seed / settings, before anything reads them.
    wwe2k14::gfx_menu::LogQualityNote();  // Session 93: the quality tier a launcher resolved (its flags carry the reason)
    wwe2k14::gfx_menu::ApplyLauncherSettings();  // Session 28: graphics settings from the launcher's file (release builds)
    if (REXCVAR_GET(wwe2k14_use_launcher_settings))  // Session 32: the DLC switches too
      wwe2k14::dlc::ApplyLauncherSettings(wwe2k14::gfx_menu::SettingsFile());
    wwe2k14::reshoot::PreSetup();  // sets --wwe2k14_replay_play etc. first (no-op unless --wwe2k14_reshoot)
    wwe2k14::director::PreSetup();  // Session 17: scenario / seed / replay mode from a director script (if any)
    wwe2k14::replay::g_replay.PreSetup();
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";

    // Load the GPU plugin here (the SDK would do the same right after this hook)
    // so its cvars are registered before graphics initialises.
    wwe2k14::blackbox::Step("set-up: loading the GPU plugin");
    if (!config.graphics) {
      const std::string backend = REXCVAR_GET(wwe2k14_gpu_backend);
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      if (!config.graphics && backend != "any") {
        REXLOG_WARN("[wwe2k14] GPU backend '{}' unavailable in plugin '{}'; falling back to 'any'", backend,
                    config.gpu_plugin);
        config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "any");
      }
    }
    wwe2k14::xdet_check::AfterGpuPlugin();  // Session 77 (P7): the profile's GPU-library settings
    // Session 54: the settings file once more now that the GPU plugin's settings exist (gpu_native, the native
    // renderer's flags, gpu_native_truth_draw_height: set before this they were lost - run 3652 ran without NATIVE;
    // likely also run 2139's resolution_scale). Only what is still at default (the first call's settings stay).
    wwe2k14::gfx_menu::ApplyLauncherSettings();

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
#if defined(__ANDROID__)
    // Session 63 (Android port): the separate submit thread stays on, as on the PC. Adreno never wakes a fence wait whose
    // work the submit thread has not handed to the driver yet (runs 007-008 stalled); the SDK now drains the submit
    // queue before such a wait on Android (command_processor.cpp AwaitSubmissionCompletion). Played match: 39.7 fps
    // mean without the thread (run 017) -> 47.7 with it (run 020), lowest 5 s window 16.9 -> 37.1.
    // Session 63 (Jarrod, ~18:45: "all the current default settings should be built right into the android build"):
    // there is no launcher and no settings file on the phone, and a +dev build never applies the launcher's defaults
    // (gfx_menu::ApplyLauncherSettings), so the PC's defaults are set here: the NATIVE renderer's list (with its
    // no-interlock part, gpu_native_truth_host_rt: Adreno has no interlock) and the host render-target path's two depth
    // fixes. The 1440p60 preset below adds no MSAA / 60 fps entrances / UPSKIP / TEXDIRECT; its resolution stays 720p
    // here until Jarrod picks the phone's picture (ANDROID_PLAN F5). Each only if left at default.
    if (rex::cvar::GetFlagSource("resolution_scale") == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName("resolution_scale", "1");
    // Session 69: readback_resolve_batch (Session 66, H1.1; exact on the PC) was left out here: on the phone it left
    // holes in Undertaker's back, white and see-through, in every b37 match (runs 059, 061, 063-064, 066-067); off,
    // clean (068). Same bytes, later copy: either ARM's weaker store ordering or a guest read the batch's flush points
    // miss (unconfirmed).
    // Session 73, b79 (Jarrod: "Try 1"): back on. Without it every resolve in a readback window (a character file
    // loaded, the in-game loads: create_readback.h) waits for the whole GPU: Jarrod's b78 log (report 119) has every
    // slow stretch at an entrance / match start inside such a window, each frame a "copy" wait (37 frames at the first
    // entrance, 172 at the second with 4.4 s waited), ending when the waits end. Since part 1 the PPC sync / lwsync /
    // eieio are real fences on ARM (they were no-ops: the suspected store-ordering cause). Watch Undertaker's back.
    // b80 (Jarrod's b79 run: "White marks present on taker"): the guest fences were not the cause; off again.
    // b81: on again with the SDK's host-side fence (FlushPendingReadbacks): the batch's copies and the signal written
    // right after them were plain stores, in order on x86, not on ARM. The PC with its GPU side lagging
    // (gpu_debug_swap_delay_ms 20, batch on, run 4780) shows no marks: no copy is read before its signal.
    // Jarrod's b81 run: "white patches still present" - off again (= b80); the cause is Session 74's (on the phone).
    // Session 74: gpu_native_truth_color_none (LOAD_OP_NONE / STORE_OP_NONE truth passes) left out too. The phone
    // launcher has passed =false since b71 (the static with fill-the-screen), but adb test runs (args.txt) got it on:
    // on the Adreno 830 it gave white patches on Undertaker's back and black flashes through the match (phone runs
    // 076-080 white; 081, only it off, clean). Off here so test runs and players' runs match.
    // Session 74 (phone on adb, Jarrod judging): the batch on again, with small one-off resolves read back as before
    // (SDK readback_resolve_batch_min_bytes 16385). With the whole batch, the game's per-character cube maps (six 64x64
    // faces, 16 KB each, one copy per slot at the end of the load) had faces 2-6 holding the scratch cube's last
    // pixels instead of their own renders (resolve hashes, runs 087 vs 088; why the batch changes those renders is not
    // known) - Taker's white marks. batch_max=1 was clean (090) but brought the hitches back; all small copies
    // synchronous was clean but kept ~20 frames of 35-45 ms from the 924 recurring small copies (098). One-off only:
    // clean, and the load window's frames as with the whole batch: 4 over 33 ms vs 70 without it (099 / 100).
    for (auto [name, value] : {std::pair{"readback_resolve_batch", "true"},
                               std::pair{"readback_resolve_batch_min_bytes", "16385"}}) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    }
    // (Session 80 kept gpu_native_truth_carry_copies / serve_copies off here: they lit the match crowd wrong on the
    // Adreno 830, cause not found. Session 84, Jarrod: "ideally we don't need the workaround anymore" - removed: the
    // match is owned now (its lighting map copy is the owned frame's own), and the entrance check of the build without
    // it decides; the copies' settings are the PC's again.)
    for (const auto& [name, value] : wwe2k14::gfxs::NativeRendererFlags()) {
      // (Session 80: gpu_native_truth_color_none on again - its bug fixed (one-draw passes were given LOAD_OP_NONE /
      // STORE_OP_NONE), phone run 164 clean with it on; Jarrod: "rebuild the APK safely with it on".)
      if (name == "readback_resolve_batch") continue;
      // (Session 82: the owned frame was PC-only until a phone run of it. Session 84, Jarrod: "Yes, on for phones" - on
      // the host render target path after SDK 3a3e130 / f527e63; phone runs 187 / 188 on the Adreno 830: no visual
      // errors, GPU 11.7 vs 12.9 ms a match frame.)
      // (Session 72, Jarrod: "build into the android version": gpu_native_shader_swap included - our copy of the post
      // pass; first run on a phone GPU is his, android-v0.1.2.)
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    }
    wwe2k14::light::ApplyFixes();
    // Session 73: Mali GPUs on their own drivers have no vertex shader stores and no line / point fill modes
    // (vulkan.gpuinfo.org, every Mali report), and the SDK refused them outright, so Mali phones could not start the
    // game. Neither is needed here: no vertex shader of the boot or a match exports memory (emulated Mali on the PC,
    // runs 4634 / 4635: no [vs-memexport] line, clean match) and line / point fills fall back to solid.
    for (auto [name, value] : {std::pair{"vulkan_require_vertex_pipeline_stores_and_atomics", "false"},
                               std::pair{"vulkan_require_fill_mode_non_solid", "false"},
                               // Session 95 (Lane D): geometryShader is not part of the phone baseline. Many Mali / PowerVR phones lack it;
                               // the primitive processor expands rectangle / point lists in the vertex shader without it.
                               std::pair{"vulkan_require_geometry_shader", "false"}}) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    }
    REXLOG_INFO("[wwe2k14][android] PC defaults built in: gpu_native={}, truth={}, truth_host_rt={}, resolution_scale={}",
                rex::cvar::GetFlagByName("gpu_native"), rex::cvar::GetFlagByName("gpu_native_truth"),
                rex::cvar::GetFlagByName("gpu_native_truth_host_rt"), rex::cvar::GetFlagByName("resolution_scale"));
#endif
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
    // Session 79 (HD 520 arc): --wwe2k14_native_frame, the arc's switch (src\native_frame.h).
    wwe2k14::native_frame::Apply();
    // Session 87: --wwe2k14_d3d_owned, the whole D3D-layer stack in one experimental switch (src\d3d_owned.h).
    // Session 94 (idea 12): --wwe2k14_low_ram, smaller sizes on a phone under 6 GB; before d3d_owned::Apply so its 1024 MB budget yields.
    wwe2k14::low_ram::Apply();
    wwe2k14::d3d_owned::Apply();
    // Session 95: the lean set, after d3d_owned::Apply: TRUE NATIVE on a PC or a phone turns it on there (a flag set by hand keeps its value).
    if (REXCVAR_GET(wwe2k14_s95_lean)) {
      for (auto [name, value] : {std::pair{"vulkan_upload_hoist_merge", "true"},
                                 std::pair{"vulkan_hoist_tight_barrier", "true"},
                                 std::pair{"kernel_cs_spin_pause", "8"},
                                 std::pair{"kernel_timestamp_tick_ms", "4"},
                                 std::pair{"present_ui_idle_wait_ms", "8"},
                                 std::pair{"wwe2k14_low_power_spin_us", "0"},
                                 std::pair{"wwe2k14_low_power_sleep_us", "500"},
                                 std::pair{"wwe2k14_load_turbo_vblank_hz", "4000"}}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
      REXLOG_INFO("[s95][lean] wwe2k14_s95_lean on: hoist merge + tight barrier, cs pause 8, timer 4 ms, UI wait 8 ms, "
                  "low power spin 0 / sleep 500 us, load turbo 4 kHz");
    }
    // Session 93 (idea 14): --wwe2k14_android_pace_gate and the start lines of the pacing switches (src\android_pace.h).
    wwe2k14::android_pace::Apply();

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
    // Session 73 (Jarrod's idea 6, a tester's phone at 23 fps in matches): the match's crowd shader 183CC64A4C4E96AC
    // (a third of a match frame's draws, docs/systems/frame_anatomy.md) was never on the list, so the scene crowd's
    // batching made no instances in matches (every member refused: "no merge stride"). With it: 70-74 of ~92 queued
    // members a frame batched in the match, same-frame replay pixel-identical (run 4709 vs 4664).
    if (REXCVAR_GET(wwe2k14_s23_defaults)) {
      for (auto [name, value] :
           {std::pair{"vulkan_repeat_draw_fast", "true"}, std::pair{"vulkan_repeat_draw_merge", "true"},
            std::pair{"vulkan_repeat_draw_merge_shaders", "D5E2FCC9F76BC40C,C1028A64EFCF8229,183CC64A4C4E96AC"}}) {
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
    // Session 83 (GitHub #79, an Intel Iris Xe laptop at 60 Hz: tearing in matches): the SDK presents IMMEDIATE when
    // the driver offers it (for variable refresh), so every screen without G-SYNC / FreeSync tore. With vsync on (every
    // player's default) mailbox instead: every frame shown, no tearing, no wait (Session 22 at 165 Hz, Jarrod: "no
    // tearing, exceptionally smooth"). vsync off (fast-forwarded test runs) keeps immediate. Only if left at default.
    // v0.2.17 release check (2026-11-03, Jarrod: "Ship without it"): MAILBOX made entrances choppier on the PC (frames
    // over 20 ms 128 / 285 vs 32 / 33 with IMMEDIATE, release.ps1 entrance check, build logs 026 / 027): off by default
    // (--wwe2k14_present_mailbox) until that is fixed.
    if (REXCVAR_GET(wwe2k14_present_mailbox)) {
      const std::string vsync = rex::cvar::GetFlagByName("vsync");
      if (vsync != "false" && vsync != "0" &&
          rex::cvar::GetFlagSource("vulkan_allow_present_mode_immediate") == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", "false");
      }
    }
    // Session 95 (F6): gpu_d3d_decouple is init-only and read when the device is made (vulkan_device.cpp), so it is set here, before it. Only if left at default.
    if (REXCVAR_GET(wwe2k14_decouple_120)) {
      for (auto [name, value] : {std::pair{"gpu_d3d_decouple", "9"}, std::pair{"gpu_d3d_decouple_word", "0"}}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      }
      // The game steps at its own 60 in this mode: a launcher FRAME RATE 2 (120 steps a second) arriving with it would run both (120 steps AND 2 views), so its step flags are cleared.
      for (auto [name, value] : {std::pair{"wwe2k14_step_hz", "0"}, std::pair{"wwe2k14_step_hz_entrance", "0"}, std::pair{"wwe2k14_step_vblank", "false"}}) {
        if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) {
          REXLOG_INFO("[wwe2k14][decouple] wwe2k14_decouple_120 clears {} (was {})", name, rex::cvar::GetFlagByName(name));
          rex::cvar::SetFlagByName(name, value);
        }
      }
      REXLOG_INFO("[wwe2k14][decouple] wwe2k14_decouple_120: gpu_d3d_decouple = {}, word = {} (60 game steps a second, 120 pictures)",
                  rex::cvar::GetFlagByName("gpu_d3d_decouple"), rex::cvar::GetFlagByName("gpu_d3d_decouple_word"));
    }
    // Session 31: vulkan_device = the dedicated GPU (or --wwe2k14_gpu), if not set (after the plugin registered it).
    if (REXCVAR_GET(wwe2k14_gpu_backend) != "d3d12") wwe2k14::gpu_pick::Choose();
#if !defined(__ANDROID__)
    wwe2k14::d3d_owned::ApplyIntegrated(wwe2k14::gpu_pick::g_known && wwe2k14::gpu_pick::g_chosen.integrated(), wwe2k14::gpu_pick::g_chosen.name, wwe2k14::gpu_pick::g_chosen.local_mb, wwe2k14::CpuIsHybrid());  // Session 95
#endif
    wwe2k14::intel_exact_mul::Configure();  // Session 76: an Intel tester's black eyes in entrances (unconfirmed cause)
    wwe2k14::light::ConfigureFallback();  // Session 37: the card's render path in the log (+ --wwe2k14_fbo_fallback_fix)
    wwe2k14::low_power::Configure();      // Session 37: --wwe2k14_low_power (default off)
    wwe2k14::display_mode::Configure();  // Session 95: nothing unless --wwe2k14_display_720p (before the window exists)
    wwe2k14::SlowFrameOnPreSetup();  // Session 8 task 12: nothing unless --wwe2k14_slowframe_ms > 0
    wwe2k14::dlaa::Configure();      // Session 18: DLAA phase 1 (all off unless a --wwe2k14_dlaa_* flag is set)
    wwe2k14::gfx_menu::ConfigureTextureFilter();  // Session 33: --wwe2k14_texture_filter (0 = the game's own, S80)
    wwe2k14::ultrawide::Configure();  // Session 33: --wwe2k14_aspect (16:9 = unchanged)
    // Session 33: photo mode is built on the CineCam free camera.
    if (REXCVAR_GET(wwe2k14_photo_mode) && rex::cvar::GetFlagSource("wwe2k14_cinecam") == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName("wwe2k14_cinecam", "true");
    wwe2k14::gfx_menu::CaptureRunning();  // Session 28: what this run does, for the in-game GRAPHICS menu
  }

  void OnPostInitLogging() override {
    // Session 73: the black box first, and its crash recorder before the SDK's fault handler (runtime set-up).
    wwe2k14::blackbox::Open();
    wwe2k14::blackbox::InstallCrashRecorder();
    wwe2k14::blackbox::Step("logging up");
#if defined(__ANDROID__)
    // Session 63: --wwe2k14_android_pin_cpu=N runs the game on CPU core N only (this thread now; every thread made
    // after it inherits). A diagnosis switch: on one core the CPU's memory ordering cannot show (phone run 013's freeze).
    if (const int cpu = REXCVAR_GET(wwe2k14_android_pin_cpu); cpu >= 0) {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(cpu, &set);
      const int r = sched_setaffinity(0, sizeof(set), &set);
      REXLOG_INFO("[wwe2k14][android] pinned to CPU {}: {}", cpu, r == 0 ? "ok" : std::strerror(errno));
    }
#endif
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
    wwe2k14::blackbox::Step("set-up done: memory, kernel, GPU device, game files mounted");
    wwe2k14::owner_timing::StartEarly(REX_KERNEL_MEMORY()->virtual_membase());  // lane 260 (3e): the thread that ticks the vblank must exist before the first fence / flip item (no-op unless --wwe2k14_owner_vblank_ours took effect)
    // Session 100 stage B: the window the new renderer presents to (the SDK's own SDL window; the game's SDL copy cannot see it).
    if (REXCVAR_GET(wwe2k14_rdr_owner) && window()) {
      wwe2k14::rdr_reader::g_owner_window = window()->GetNativeWindowHandle();
#if defined(__ANDROID__)
      // Lane 235: Android destroys the ANativeWindow in the background and makes a new one on return; our swapchain (owner mode) asks
      // for the current one every frame and follows SDL's app-state events (the SDK's own fix, f5d75cd, only reaches its presenter).
      wwe2k14::rdr_reader::g_owner_window_source = [](void* user) -> void* {
        return static_cast<rex::ui::Window*>(user)->GetNativeWindowHandle();
      };
      wwe2k14::rdr_reader::g_owner_window_source_user = window();
      wwe2k14::android_surface_events::Install();
#endif
#if defined(_WIN32)
      wwe2k14::rdr_reader::g_owner_instance = GetModuleHandleW(nullptr);
#endif
      // Settings parity S11 (SDK 88e4077): with presentation detached the SDK asks for our immediate drawer (OnCreateImmediateDrawer), so
      // OnCreateDialogs has run and installed everything, the drawer dialogs included (CineCam, inspector, check banner, performance
      // overlay, upscale guard: drawn by the new renderer). Without a drawer (an older SDK) the installs that need none run here (S100:
      // the WM30 jump never armed in owner mode, run 6894).
      if (!imgui_drawer()) InstallWithoutDrawer();
      // Photo mode saves through the new renderer (no SDK presenter in owner mode).
      wwe2k14::photo::g_owner_save = [](const std::filesystem::path& path) { return wwe2k14::rdr_reader::g_live.SavePicture(path); };
      case_hook::SetPictureSaver([](const std::filesystem::path& path) { return wwe2k14::rdr_reader::g_live.SavePicture(path); });  // S101 lane 11: the bug button's picture.png (PNG)
      rdr_case::g_drain_log = [] { rex::DrainLogging(500); };  // lane 302 (review N1): the case worker reads the log file
      case_hook::SetExitFlusher([] { wwe2k14::rdr_reader::g_live.FlushFinalCensus(500); rex::FlushLogging(); });  // S101 lane 216: every class's final census line, before the exit zip reads the log
      case_hook::SetLastPictureSaver([](const std::filesystem::path& path) { return wwe2k14::rdr_reader::g_live.SaveLastPicture(path); });  // S101 lane 19: a run exit's picture
      // S101 lane 19: the case zip's names block = the [rdr][names] counters (the census logs the same ones), sampled once per frame and summed over the window.
      rdr_case::info::SetNamesProbe([]() {
        namespace rr = wwe2k14::rdr_reader;
        rdr_case::info::NamesSample s;
        s.draws = rr::g_names.draws, s.untagged = rr::g_names.untagged, s.unclassified = rr::g_names.unclassified;
        s.surfaces_guessed = rr::g_roles.guessed;
        std::lock_guard lock(rr::g_prov.mu);
        s.textures_unnamed = rr::g_prov.by_kind[uint8_t(rr::cap::Provenance::Unnamed)];
        for (uint8_t i = 0; i < uint8_t(rr::cap::Provenance::Count); ++i) s.texture_binds += rr::g_prov.by_kind[i];
        return s;
      });
      if (imgui_drawer() && wwe2k14::rdr_overlay::g_drawer) {
        wwe2k14::rdr_reader::g_on_present = [this] {
          wwe2k14::rdr_overlay::RequestPaint([this](std::function<void()> f) { app_context().CallInUIThread(std::move(f)); }, window(),
                                            imgui_drawer());
        };
      }
    }
    // The old path keeps answering the game's fences and flips but draws nothing (SDK gpu_no_draw). Here, not in OnPreSetup: the GPU
    // plugin that defines the switch is loaded after OnPreSetup. Left alone if set on the command line.
    if (REXCVAR_GET(wwe2k14_rdr_owner) && !wwe2k14::GuestGpuOnly() && rex::cvar::GetFlagSource("gpu_no_draw") == rex::cvar::Source::kDefault) {  // (lane 241: nothing old to switch off under guest_gpu_only)
      const bool set = rex::cvar::SetFlagByName("gpu_no_draw", "true");
      REXLOG_INFO("[rdr][live] owner mode: the old path draws nothing (gpu_no_draw {})", set ? "on" : "NOT FOUND: the old path still draws");
    }
    // Session 95 (flat guest memory): this exe's recompiled code and hooks add the offset they were compiled with; the
    // SDK's heap says what the mapping really is. An exe and a rexruntime.dll from different builds would read every
    // physical allocation 4 KB off, silently: stop here instead. (The generated macro against the header: the
    // static_assert above the class.)
    if (const auto* e0 = runtime()->memory()->LookupHeap(0xE0000000u);
        e0 && e0->host_address_offset() != ::rex::memory::detail::PhysicalHostOffset(0xE0000000u)) {
      REXLOG_CRITICAL("[wwe2k14] this exe and rexruntime.dll come from different builds (0xE0000000 host offset {:#x} vs {:#x}): stopping",
                      ::rex::memory::detail::PhysicalHostOffset(0xE0000000u), e0->host_address_offset());
      rex::FlushLogging();  // lane 302: the log is written by a thread; _Exit would lose the CRITICAL line
      std::_Exit(3);
    }
    REXLOG_INFO("[wwe2k14] game_data_root: {} ({})", game_data_root().string(),
                std::filesystem::is_regular_file(game_data_root()) ? "disc image" : "folder");
    REXLOG_INFO("[wwe2k14] user_data_root: {}", user_data_root().string());  // Session 53: the save folder in use
    wwe2k14::xdet_check::PostSetup(game_data_root());  // Session 77 (P7): the profile's notes, the identity line
    // S101 lane 1 (Phase C wiring): the live scene writer's class contributors register once, here, before any sink can attach
    // (live_scene.h: "Registration happens once, at startup, before Enabled() can become true"). Only when scene records are asked
    // for: with --wwe2k14_scene_records off (the default, and refused by timed / speed runs) nothing registers and every class hook
    // returns at its first check.
    if (REXCVAR_GET(wwe2k14_scene_records)) {
      wwe2k14::scene_live::InstallArena(game_data_root());  // C3: the slot provider + the arena contributor (lane 28)
      rdr_scene::live::RegisterWrestlers();                  // C4: node palettes from the skeleton updates (lane 29)
      rdr_scene::g_skeleton_want_hooks.want = [](uint32_t m, uint32_t n) { rdr_scene::live::Wrestlers().WantSkeleton(m, n); };   // lane 207: own-skeleton pieces (hair, pads) the scene core joined
      rdr_scene::g_skeleton_want_hooks.unwant = [](uint32_t m) { rdr_scene::live::Wrestlers().UnwantSkeleton(m); };
      if (REXCVAR_GET(wwe2k14_scene_light_states)) rdr_scene::live::lights::RegisterLightStates();  // S101 lane 109 (LS1): R at every model call -> kScLightStates / kScLightRefs (own switch: F2)
      if (REXCVAR_GET(wwe2k14_scene_circles) || REXCVAR_GET(wwe2k14_rdr_circles_ours)) rdr_scene::live::circles::RegisterCircles();
      if (REXCVAR_GET(wwe2k14_scene_plane) || REXCVAR_GET(wwe2k14_rdr_plane_shadow_ours)) rdr_scene::live::plane::RegisterPlane();  // lane 139 (SH3 slice 2): the plane chain constants -> kScPlaneFrames / kScPlaneDraws
      if (REXCVAR_GET(wwe2k14_scene_light_states)) rdr_scene::live::arenaworld::RegisterArenaWorlds();  // lane 130: the arena's object worlds + the stage clock -> kScArenaWorlds (rides the light-state switch: Check J1)
#if defined(__ANDROID__)
      // Lane 202 review SF1: the ring / caster D3 readers copy guest memory with SafeCopy, a plain memcpy off Windows: a stale pointer
      // would crash the game. Not on the phone until SafeCopy is fault-safe there.
      if (REXCVAR_GET(wwe2k14_rdr_ring_d3) || REXCVAR_GET(wwe2k14_rdr_caster_d3) || REXCVAR_GET(wwe2k14_rdr_wrestlers_d3))
        REXLOG_WARN("[scene][live] --wwe2k14_rdr_ring_d3 / --wwe2k14_rdr_caster_d3 / --wwe2k14_rdr_wrestlers_d3: not on Android until SafeCopy is fault-safe: off");
      if (false) {
#else
      if (REXCVAR_GET(wwe2k14_rdr_ring_d3) && REXCVAR_GET(wwe2k14_scene_light_states)) {  // lane 194 (Ring D3 R2): inert; the record only, no skip is built (ring_rows_core.h kRingSkipBuilt)
#endif
        rdr_scene::live::ringrows::RegisterRingRows();
        REXLOG_INFO("[scene][live] ring_rows registered (Ring D3 record arm: the ring's node rows are recorded; the skip is a separate switch, --wwe2k14_rdr_ring_d3_live (lane 246, 16.9): [rdr][d3][ring] lines say whether it is on{})", REXCVAR_GET(wwe2k14_rdr_ring_d3_live) ? (REXCVAR_GET(wwe2k14_rdr_ring_d3_live_verify) ? ", verify arm: never skipped" : "") : "; --wwe2k14_rdr_ring_d3_live not set: nothing is skipped");
      } else if (REXCVAR_GET(wwe2k14_rdr_ring_d3)) {
        REXLOG_WARN("[scene][live] --wwe2k14_rdr_ring_d3 needs --wwe2k14_scene_records and --wwe2k14_scene_light_states: off");
      }
#if defined(__ANDROID__)
      if (false) {
#else
      if ((REXCVAR_GET(wwe2k14_rdr_caster_d3) || REXCVAR_GET(wwe2k14_rdr_wrestlers_d3)) && REXCVAR_GET(wwe2k14_scene_light_states)) {
#endif  // lane 202 (Caster D3 E0-E2): inert; the record only, no skip is built (caster_core.h kCasterSkipBuilt). Lane 227 (Wrestlers D3 H2): the wrestlers' cut reads the SAME records (the map entries' light camera, the rim entry's depth range), so its switch registers them too
        rdr_scene::live::casters::RegisterCasters();
        rdr_scene::live::spheres::RegisterSpheres();   // lane 242 (H3): the wrestlers' sphere occluders, recorded at the exit of sub_826DF8E0 (read only; the same switches, never Android)
        if (REXCVAR_GET(wwe2k14_rdr_caster_d3))
          REXLOG_INFO("[scene][live] casters registered (Caster D3: the caster pass entries are recorded; the skip itself (lane 228, 17.12) is decided in the render thread's start-up: [rdr][d3][casters] lines say whether it is on{})", REXCVAR_GET(wwe2k14_rdr_caster_d3_verify) ? ", verify arm: never skipped" : "");
        if (REXCVAR_GET(wwe2k14_rdr_wrestlers_d3))
          REXLOG_INFO("[scene][live] wrestlers D3 registered (the colour calls' light states, the caster pass entries (light camera, rim depth) and the sphere occluder rows are recorded for the wrestlers' cut; the skip itself (lane 245, 18.9) is decided in the render thread's start-up: [rdr][d3][wrestlers] lines say whether it is on{})", REXCVAR_GET(wwe2k14_rdr_wrestlers_d3_verify) ? ", verify arm: never skipped" : "");
      } else if (REXCVAR_GET(wwe2k14_rdr_caster_d3) || REXCVAR_GET(wwe2k14_rdr_wrestlers_d3)) {
        REXLOG_WARN("[scene][live] --wwe2k14_rdr_caster_d3 / --wwe2k14_rdr_wrestlers_d3 need --wwe2k14_scene_records and --wwe2k14_scene_light_states: off");
      }
      wwe2k14::crowd_live::RegisterCrowd();                  // C2: members, type palettes, cards (lane 27; its arena from the C3 hook)
      if (REXCVAR_GET(wwe2k14_rdr_crowd_ours)) {             // S101 lane 35: the crowd's first promotion needs the LOD 0 palette of every used type, and the game folder for the bodies
        rdr_scene::live::crowd::The().SetPoseObjects(true);
        wwe2k14::crowd_live::g_floor_observe.store(REXCVAR_GET(wwe2k14_rdr_crowd_floor), std::memory_order_relaxed);  // S101 lane 82: the floor people's op 21 is observed only for the floor switch
        wwe2k14::rdr_reader::g_crowd_game_root = game_data_root().string();
      }
      if (REXCVAR_GET(wwe2k14_rdr_arena_ours) || REXCVAR_GET(wwe2k14_rdr_wrestlers_ours))  // S101 lane 41: the in-place hooks read the game folder too (the arena's meshes)
        wwe2k14::rdr_reader::g_crowd_game_root = game_data_root().string();
      REXLOG_INFO("[scene][live] classes registered: arena, wrestlers, crowd{} (scene records on)", REXCVAR_GET(wwe2k14_scene_light_states) ? ", lights, arena_worlds" : "");
    }
    // S101 Check H2 (run 7075 / 7076): every promoted class reads the player's game folder, not only the record-fed ones: the UI needs no scene
    // records, and the ring / flashes must not depend on the arena switch being on too. The root is only a path; a hook that is off never reads it.
    if (REXCVAR_GET(wwe2k14_rdr_crowd_ours) || REXCVAR_GET(wwe2k14_rdr_arena_ours) || REXCVAR_GET(wwe2k14_rdr_wrestlers_ours) || REXCVAR_GET(wwe2k14_rdr_caw_ours) /* lane 153 */ ||
        REXCVAR_GET(wwe2k14_rdr_ring_ours) || REXCVAR_GET(wwe2k14_rdr_flash_ours) || REXCVAR_GET(wwe2k14_rdr_props_ours) || REXCVAR_GET(wwe2k14_rdr_shadow_casters_ours) || REXCVAR_GET(wwe2k14_rdr_screens_ours) || REXCVAR_GET(wwe2k14_rdr_rim_ours) /* lane 124 */ || REXCVAR_GET(wwe2k14_rdr_crowd_props_ours) /* lane 133 */ || REXCVAR_GET(wwe2k14_rdr_circles_ours) /* lane 136 */ || REXCVAR_GET(wwe2k14_rdr_plane_shadow_ours) /* lane 139 */ || REXCVAR_GET(wwe2k14_rdr_ui_ours) || REXCVAR_GET(wwe2k14_rdr_ui_text_ours))  // S101 lane 84: props; lane 89
      wwe2k14::rdr_reader::g_crowd_game_root = game_data_root().string();
    wwe2k14::diag::Init();  // Session 95: the diagnostic run (no-op unless --wwe2k14_diag): its folder, cvars.txt, the watchdog
    wwe2k14::keybinds_save::Install(user_data_root());  // Session 53 item 2.6: keyboard binds kept between starts
    wwe2k14::keyboard_player::Install(runtime());  // Session 76: the keyboard as its own player (P2 beside one pad)
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
    wwe2k14::music_app::SetGameRoot(game_data_root());  // lane 298: the arena effects read the stock chain from the player's own sound bank
    wwe2k14::create_readback::Install();  // Session 57 (tester bug 4): readback in the create modes
    // Session 58: native BPE only in the in-game sequence (9) unless --wwe2k14_native_bpe_menus (src/decode_probe.h).
    wwe2k14::decode_probe::g_native_bpe_in_game = [] { return wwe2k14::load_probe::CurrentSequence() == 9; };
    wwe2k14::load_pipeline_wait::Install();  // Session 57 (tester bug 1): no skipped draws during in-game loads
    wwe2k14::post_swap::Install();  // Session 72: our post-effect shaders (used with gpu_native_shader_swap)
    wwe2k14::scene_swap::Install();  // Session 80: our own scene material shaders (--wwe2k14_scene_shaders)
    wwe2k14::scene_vs_swap::Install();  // Session 87 stage 4: our own arena vertex shaders (registered always; --wwe2k14_vs_swap turns them on)
    // Session 58: the session timeline for Report a problem (snapshots off in harness runs unless asked for).
    wwe2k14::session_log::Install(!REXCVAR_GET(wwe2k14_input_script).empty() || !REXCVAR_GET(wwe2k14_live).empty());
    wwe2k14::StartTimelineIfRequested();
    wwe2k14::StartThreadPlacement();  // logs the CPU topology; watcher only with --wwe2k14_thread_placement
    wwe2k14::StartPowerThrottle();    // Session 95: EcoQoS / core packing, nothing unless asked (power_throttle.h)
    wwe2k14::StartFlightRecorder();   // no-op unless --wwe2k14_flight_recorder (Session 8 task 11)

    // Input scripts can end the run the same way Alt+F4 does.
    wwe2k14::g_request_quit = [this] {
      app_context().CallInUIThreadDeferred([this] {
        // RequestClose closes without asking OnWindowCloseRequested (SDL: PerformClose), so write it here too.
        wwe2k14::perf_report::Write();  // no-op unless --wwe2k14_perf_report (Session 20)
        wwe2k14::display_mode::Restore("script quit");  // Session 95
        if (window()) window()->RequestClose();
      });
    };
    wwe2k14::android_lifecycle::Install(  // Session 95
        [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); });
    // Lane 256 (N1): in owner mode the SDK presenter shows nothing (or does not exist), so its capture would read a dark picture and the guard
    // would say a false "BLACK: dark picture". No capture then (Pictures() is skipped); the "no present" rule still names a black screen.
    wwe2k14::hang_guard::Start(wwe2k14::CurrentLogNumber(), REXCVAR_GET(wwe2k14_rdr_owner) ? wwe2k14::hang_guard::Capture{} : wwe2k14::hang_guard::Capture{[this](rex::ui::RawImage& image) {  // Session 95 (after diag::Init)
      auto* graphics = runtime()->graphics_system();
      auto* presenter = graphics ? graphics->presenter() : nullptr;
      return presenter && presenter->CaptureGuestOutput(image);
    }});
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
    wwe2k14::precompile::Install();   // no-op unless --wwe2k14_precompile (Session 70: the launcher's precompile)
    wwe2k14::audio_capture::Install();  // no-op unless --wwe2k14_audio_capture (Session 18)
    wwe2k14::g_input_script.Init();
    wwe2k14::live::Install();  // no-op unless --wwe2k14_live (Session 29)
  }

  // Session 20 item 9: a window close (Alt+F4) asks here first, before ReXGlue's hard exit in OnClosing: the last point
  // to write the playtester report. An input script's quit (RequestClose) skips this; g_request_quit writes it.
  bool OnWindowCloseRequested() override {
    wwe2k14::display_mode::Restore("close requested");  // Session 95
    wwe2k14::perf_report::Write();  // no-op unless --wwe2k14_perf_report
    return true;
  }

  // Session 95: the 720p display mode is given back on a window close (OnWindowCloseRequested, above) and a script quit
  // (g_request_quit); also while the game is not in front (alt-tab, minimise). (ReXApp::OnClosing is private.)
  void OnWindowFocusChanged(bool focused) override {
    // A failed re-apply leaves a 1280x720 window in the corner of the native desktop: toggle fullscreen so SDL re-reads
    // the monitor bounds.
    if (wwe2k14::display_mode::OnFocus(focused) && window()) {
      window()->SetFullscreen(false);
      window()->SetFullscreen(true);
    }
  }
#if defined(__ANDROID__)
  // Session 95 (piece LIFE): every in-game exit lands in the launcher. The SDK's OnClosing calls TerminateTitle, which crashes on
  // the phone (r014, QUIT TO DESKTOP: "QueueUserCallback: signal delivery failed", then CRASH_NATIVE status 6). Nothing here stops
  // the title: GameActivity's poll sees the request within 400 ms and ends the process; _Exit is the backstop.
  void OnClosing(rex::ui::UIEvent& e) override {
    (void)e;
    REXLOG_INFO("[life] window closing: quit to the launcher");
    wwe2k14::blackbox::Note("quit to the launcher (the game closed its window)");
    rex::RunQuitFlushes(400);  // S101 lane 216: the final census lines (the SDK's OnClosing does this on the PC; this override replaces it)
    rex::FlushLogging();
    wwe2k14::android_lifecycle::WriteExitRequest("the game closed its window");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::_Exit(0);
  }
#endif

  // Only on ReXGlue's OnDestroy path; the usual close hard-exits (std::_Exit in ReXApp::OnClosing) before this.
  void OnShutdown() override {
    wwe2k14::owner_timing::RequestStop();  // lane 260: no more vblanks dispatched from our clock into the tearing-down runtime
    wwe2k14::display_mode::Restore("shutdown");  // Session 95: guest exit / setup failure paths
    wwe2k14::StopThreadPlacement();
    wwe2k14::StopPowerThrottle();
    wwe2k14::inspector::Shutdown();  // no-op unless --wwe2k14_inspector
    wwe2k14::cinecam::Shutdown();    // no-op unless --wwe2k14_cinecam
    wwe2k14::check::Shutdown();      // Session 29: no-op outside harness runs
    wwe2k14::music_picker::Shutdown();  // lane 291
    wwe2k14::bookmark::Shutdown();   // Session 29
    wwe2k14::audio_capture::Shutdown();  // Session 18
  }

  // Session 8 task 15: ImGui live inspector (src/inspector.h). Returns at once unless --wwe2k14_inspector=true.
  // Settings parity S11: with the new renderer owning the window (presentation detached), our immediate drawer records the ImGui dialogs
  // for it (src\renderer_adapter\overlay_drawer.h). Otherwise none: the SDK presenter's own drawer.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (!REXCVAR_GET(wwe2k14_rdr_owner)) return nullptr;
    auto drawer = std::make_unique<wwe2k14::rdr_overlay::Drawer>(wwe2k14::rdr_reader::g_live);
    wwe2k14::rdr_overlay::g_drawer = drawer.get();
    REXLOG_INFO("[rdr][overlay] the new renderer draws the app's dialogs");
    return drawer;
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    ApplyWindowIcon();  // Session 17: game_logo.png, embedded by the build (no-op without it)
    wwe2k14::inspector::Install(drawer, PPCImageConfig.code_base, PPCImageConfig.code_size, PPCImageConfig.image_base,
                                PPCImageConfig.image_size,
                                [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); });
    wwe2k14::cinecam::Install(window(), drawer);  // Session 13 CineCam free camera; no-op unless --wwe2k14_cinecam
    // Session 37: a failed FSR is turned off with a notice (no-op unless --wwe2k14_fsr).
    wwe2k14::upscale_guard::Install(
        drawer, [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); });
    // Session 29: `check TEXT` banner + F1 / F2 / F5, only in harness runs (an input script or the live channel).
    const bool harness = !REXCVAR_GET(wwe2k14_input_script).empty() || !REXCVAR_GET(wwe2k14_live).empty();
    wwe2k14::check::Install(
        drawer, [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); }, harness,
        window() ? window()->GetNativeWindowHandle() : nullptr);
    // Session 80: the performance overlay (no-op unless --wwe2k14_perf_overlay).
    wwe2k14::music_picker::Install(  // lane 291: the USER PLAYLIST song picker (a no-op without the mods folder)
        drawer, [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); });
    wwe2k14::perf_overlay::Install(
        drawer, [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); });
    InstallWithoutDrawer();
  }

  // The installs that need no overlay drawer: from OnCreateDialogs, or from OnPostSetup when the new renderer owns the window (no drawer).
  void InstallWithoutDrawer() {
    const bool harness = !REXCVAR_GET(wwe2k14_input_script).empty() || !REXCVAR_GET(wwe2k14_live).empty();
    wwe2k14::runin::InstallBind();  // Session 16: F9 = run-in (no-op unless --wwe2k14_runin)
    wwe2k14::photo::Install();  // Session 33 photo mode; no-op unless --wwe2k14_photo_mode
    wwe2k14::typing::Install(window());  // Session 55; no-op unless --wwe2k14_keyboard_typing
    wwe2k14::mouse_menu::Install(window());  // Session 67 (the cvar is checked per event: live switchable)
    wwe2k14::wm30::Install();   // Session 34; no-op unless --wwe2k14_wm30
    wwe2k14::bookmark::Install(harness);  // Session 29: F11 = "that, just now" bundle (Session 58: players too)
    wwe2k14::g_d3_latch_hook = &wwe2k14::bookmark::TakeD3Latch;   // lane 301: a D3 latch takes a case zip at the moment (renderer_adapter/d3_latch_hook.h)
    wwe2k14::screen_text::Enable(harness);  // Session 30: text the game fetched, for scripts / live / bookmarks
  }

  // Image is loaded and functions registered, guest code not started yet.
  void OnPreLaunchModule() override {
    wwe2k14::blackbox::Step("the game's code loaded; preparing its first thread");
    wwe2k14::ScanForUnregisteredFunctionPointers(runtime(), PPCImageConfig, 2000);
    DumpRequestedWords();
    wwe2k14::DumpGuestImage(runtime(), PPCImageConfig, REXCVAR_GET(wwe2k14_dump_image));  // empty = off
    // Session 61: the shader list's shaders and pipelines into the storage the SDK loads right after this.
    wwe2k14::shader_list::Import(runtime()->cache_root());
    wwe2k14::rdr_reader::g_caw_cache_root = (runtime()->cache_root() / "caw").string();   // lane 153: the CAW edit-part index cache lives beside the shader caches
    wwe2k14::rdr_reader::g_pipeline_store_root = (runtime()->cache_root() / "renderer_pipelines").string();   // lane 276 (used only with --wwe2k14_rdr_pipeline_store)
    if (!REXCVAR_GET(wwe2k14_rdr_pipeline_store_dir).empty()) wwe2k14::rdr_reader::g_pipeline_store_root = REXCVAR_GET(wwe2k14_rdr_pipeline_store_dir);   // tests: a folder that survives run_list's per-run mirror
    // Session 80 (HD 520 arc): with NATIVE's truth frames the emulated path draws nothing, so its stored pipelines are
    // not made at boot (the Surface's UHD 620: 406 stored ones at 2.6 s each, ~3 minutes before the first frame; none
    // used after). The SDK's own test for truth frames (native_renderer.cpp: refused with EDRAM renaming, or DLSS /
    // DLAA without gpu_native_truth_upscalers). Their records stay; one needed later is made on first use.
    {
      auto on = [](const char* name) {
        const std::string v = rex::cvar::GetFlagByName(name);
        return v == "true" || v == "1";
      };
      const bool upscaler_refuses = (on("gpu_dlss") || on("gpu_dlaa")) && !on("gpu_native_truth_upscalers");
      const bool truth = rex::cvar::GetFlagByName("gpu_native") == "2" && on("gpu_native_truth") &&
                         !on("gpu_edram_rename") && !upscaler_refuses;
      if (truth && rex::cvar::GetFlagSource("vulkan_stored_pipelines_at_boot") == rex::cvar::Source::kDefault)
        rex::cvar::SetFlagByName("vulkan_stored_pipelines_at_boot", "false");
    }
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
    // Session 83: a host folder reports a 64 MB volume (128 K units of 512 bytes, host_path_device.h), under the 157 MB
    // free that MATCH HIGHLIGHTS checks for before it creates cache:\Spectacular.rps; report a 2 GB one like the 360's.
    struct CacheDevice : rex::filesystem::HostPathDevice {
      using HostPathDevice::HostPathDevice;
      uint32_t total_allocation_units() const override { return 4 * 1024 * 1024; }
      uint32_t available_allocation_units() const override { return 4 * 1024 * 1024; }
    };
    auto device = std::make_unique<CacheDevice>(kMountPath, host_path, false);
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
