// wwe2k14 - Session 77 (XDET_PLAN.md "P7 testers", step 1): --wwe2k14_xdet_check, the profile of the tester
// determinism check (Jarrod's interview 2026-10-31 ~15:30: phone and PC testers, a Help button, the results in the zip
// they already send, compared at my end against a PC play of the same release).
//
// One flag makes a run comparable with my reference whatever the tester's own setup: it sets the "as played" values
// (tools\xdet\played.flags), the hash, and everything else a tester's machine or settings could change that reaches
// the guest, at the highest setting priority (kRuntime: over the command line, a wwe2k14.toml next to the exe, REX_*
// environment variables and the launcher's settings file). Evidence for each pin: the Session 77 tester-machine map
// (workflow wf_04521979-0dd, four readers + Opus skeptics; SESSION77_SUMMARY.md "Tester check, step 1"):
//   - the launcher's settings file (graphics, frame rate, DLC switches) is read by the game itself in release builds
//     (gfx_menu.h ApplyLauncherSettings, dlc.h): wwe2k14_use_launcher_settings=false;
//   - a wwe2k14.toml next to the exe (the SDK's settings overlay writes one) is not loaded at all (config_path moved);
//   - the phone launcher's FRAME RATE 30 passes --wwe2k14_step_hz=30: step 0 pinned;
//   - host input outside the recording: the menu mouse writes the guest's menu cursor (mouse_menu.h; phone touches
//     arrive as mouse events), the PC keyboard types into the game's keyboard (keyboard_typing.h), XAM dialogs take
//     the tester's click (xam_ui.cpp, headless): all off;
//   - host-answered values: user_language / user_country / license_mask, the audio queue depth, the clock options;
//   - the tester's clock and date (det_time_constant) and file times / folder-vs-image answers (det_fs_constant):
//     SDK pin-b747390 (include/rex/det.h);
//   - log_level info: the comparer needs the INFO lines "run log number" and "recording ended".
// Render-side platform defaults (readback_resolve_batch, depth_float24_convert_in_pixel_shader,
// execute_unclipped_draw_vs_on_cpu, the replay guard) stay as each device plays: phone = PC in every game layer with
// them differing (Session 77 runs 120-158); the comparer lists them.
// The launcher passes the paths, the recording (--wwe2k14_replay_play / _dir), the route (--wwe2k14_input_script) and
// a fresh --user_data_root (P7 steps 2-4). The identity line ("# xdet identity", frame_hash.h) says which build, game
// files and profile a CSV came from, and how many of the tester's values the profile replaced.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "game_files.h"

REXCVAR_DEFINE_BOOL(wwe2k14_xdet_check, false, "wwe2k14",
                    "Session 77: the tester determinism check's profile - the as-played values, the hash and every "
                    "tester-dependent input pinned over any other source (src/xdet_check.h)");

namespace wwe2k14::xdet_check {

struct Pin {
  const char* name;
  const char* value;
};

inline constexpr Pin kPins[] = {
    {"wwe2k14_use_launcher_settings", "false"},
    {"wwe2k14_det_hash", "default,objects,sim"},
    {"wwe2k14_det_hash_v2", "true"},
    // tools\xdet\played.flags (the as-played config, Jarrod 2026-10-31 ~11:10)
    {"wwe2k14_step_hz", "0"},
    {"wwe2k14_step_hz_entrance", "0"},
    {"wwe2k14_entrance_60fps", "true"},
    {"user_language", "1"},
    {"wwe2k14_dlc", "false"},
    {"wwe2k14_dynamic_res", "false"},
    {"wwe2k14_phys_alloc_fast_fail", "true"},
    {"det_serial_vblank_timeout_ms", "60000"},
    {"wwe2k14_native_skin", "true"},
    {"wwe2k14_nf_skin_3bone", "on"},
    {"wwe2k14_scene_crowd", "1"},
    {"occlusion_query_enable", "false"},  // (GPU library: set once it is loaded)
    {"resolution_scale", "1"},            // (GPU library)
    // what else a tester's machine or settings could change (the map above)
    {"wwe2k14_mods", "false"},
    {"user_country", "103"},
    {"license_mask", "0"},
    {"wwe2k14_mouse_menus", "false"},
    {"wwe2k14_keyboard_typing", "false"},
    {"headless", "true"},
    {"log_level", "info"},
    {"clock_no_scaling", "false"},
    {"clock_source_raw", "false"},
    {"audio_maxqframes", "8"},
    {"wwe2k14_load_pipeline_wait", "true"},
    {"det_time_constant", "true"},
    {"det_fs_constant", "true"},
    {"allow_game_relative_writes", "false"},  // (review) a writable game folder answers unlike a disc image
    {"gpu_det_skip_host_draws", "false"},  // (GPU library) drawn: the tester watches it (interview)
    {"vsync", "false"},                    // (GPU library) fast-forwarded (host-only)
};

inline bool On() { return REXCVAR_GET(wwe2k14_xdet_check); }

struct State {
  std::vector<std::string> notes;  // logged at OnPostSetup (logging is not configured at the first hook)
  std::vector<const Pin*> pending;  // not registered yet (the GPU library's)
  int replaced = 0;                 // tester values the profile replaced
  bool applied = false;             // the pins were set at the first hook (not: the flag came from a config file)
  std::string identity;             // the "# xdet identity" values (frame_hash.h)
};
inline State& S() {
  static State s;
  return s;
}

inline void ApplyPin(const Pin& p, bool last_chance) {
  const auto source = rex::cvar::GetFlagSource(p.name);
  const std::string before = rex::cvar::GetFlagByName(p.name);
  if (!rex::cvar::SetFlagByName(p.name, p.value)) {
    if (!last_chance) {
      S().pending.push_back(&p);
    } else {
      S().notes.push_back(std::string("could not set ") + p.name + "=" + p.value + " (no such setting)");
    }
    return;
  }
  if (source != rex::cvar::Source::kDefault && before != p.value) {
    ++S().replaced;
    S().notes.push_back(std::string("replaced ") + p.name + "=" + before + " (not the default) with " + p.value);
  }
}

// The first app hook (after the command line, before the config file and logging): every pin that exists, and no
// wwe2k14.toml. Wwe2k14App::OnConfigurePaths.
inline void ConfigurePaths(rex::PathConfig& paths) {
  if (!On()) return;
  S().applied = true;
  for (const Pin& p : kPins) ApplyPin(p, false);
  paths.config_path = paths.user_data_root.empty() ? std::filesystem::path()
                                                   : paths.user_data_root / "xdet_check_no_config.toml";
}

// Right after the GPU library loads (its settings exist now): the rest. Wwe2k14App::OnPreSetup.
inline void AfterGpuPlugin() {
  if (!S().applied) return;
  std::vector<const Pin*> pending;
  pending.swap(S().pending);
  for (const Pin* p : pending) ApplyPin(*p, true);
}

// The profile's value for a setting, or nullptr (not pinned, or the profile is off). replay.h Want: a GPU-library
// setting the profile pins after the library loads is judged by the pin, not by a command-line value it replaces.
inline const char* PinnedValue(const std::string& name) {
  if (!S().applied) return nullptr;
  for (const Pin& p : kPins)
    if (name == p.name) return p.value;
  return nullptr;
}

// 64-bit FNV-1a of default.xex through the game's file system (a folder or a disc image alike).
inline std::string XexDigest() {
  auto* entry = game_files::Resolve("game:\\default.xex");
  if (!entry) return "missing";
  const size_t size = entry->size();
  std::vector<uint8_t> buf(1u << 20);
  uint64_t h = 1469598103934665603ull;
  size_t done = 0;
  while (done < size) {
    const size_t n = game_files::Read("game:\\default.xex", std::span<uint8_t>(buf.data(), buf.size()), done);
    if (n == 0) break;
    for (size_t i = 0; i < n; ++i) h = (h ^ buf[i]) * 1099511628211ull;
    done += n;
  }
  char out[64];
  std::snprintf(out, sizeof(out), "%016llx/%zu%s", static_cast<unsigned long long>(h), size,
                done == size ? "" : "/short");
  return out;
}

// OnPostSetup (the game files are mounted): the identity values and the notes. Every run (the line costs one read of
// default.xex only with the det hash v2 on).
inline void PostSetup(const std::filesystem::path& game_data_root) {
  State& s = S();
  for (const std::string& n : s.notes) REXLOG_WARN("[wwe2k14][xdet_check] {}", n);
  // (Review: the flag from a wwe2k14.toml is seen only after the first hook - nothing was pinned.)
  if (On() && !s.applied)
    REXLOG_WARN("[wwe2k14][xdet_check] --wwe2k14_xdet_check came from a config file, too late to pin anything: give "
                "it on the command line (identity profile=late)");
  if (rex::cvar::GetFlagByName("wwe2k14_det_hash_v2") != "true") return;  // (defined in frame_hash.h, after this)
  std::string id = "version=";
#ifdef WWE2K14_VERSION
  id += WWE2K14_VERSION;
#else
  id += "-";
#endif
  std::error_code ec;
  id += std::string(" game=") + (std::filesystem::is_regular_file(game_data_root, ec) ? "image" : "folder");
  id += " xex=" + XexDigest();
  id += std::string(" profile=") + (s.applied ? "xdet_check" : On() ? "late" : "off");
  id += " replaced=" + std::to_string(s.replaced);
  id += " cores=" + std::to_string(std::thread::hardware_concurrency());
  s.identity = id;
  REXLOG_INFO("[wwe2k14][xdet_check] identity: {}", id);
}

inline const std::string& Identity() { return S().identity; }

}  // namespace wwe2k14::xdet_check
