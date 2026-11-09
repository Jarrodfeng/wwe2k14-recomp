// wwe2k14 phone launcher - its settings and the game's command line (Session 71, ANDROID_PLAN F4).
//
// The settings live in <app files>/launcher_settings.json (the app's own folder; the PC launcher's file is
// %APPDATA%\wwe2k14-launcher\settings.json). Keys that mean the same as the PC's keep the PC's names (resolution,
// draw_height, frame_rate, texture_filter, aspect, language, dlc, dlc_off, check_updates).
//
// The flags follow the PC launcher's mapping (src\gfx_settings.h GraphicsFlags, src\launcher\launcher_core.h
// GameFlags) for the choices a phone gets. gfx_settings.h itself is not included: on Android it pulls in the game
// runtime's logging (src\platform\compat.h), which this library does not link. The Android game already applies the
// NATIVE renderer's list and 720p by itself (src\wwe2k14_app.h, Session 63), so nothing here repeats them.
#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mlauncher {

namespace fs = std::filesystem;

// The phone's resolutions (gfx_settings.h kResolutions, the ones a phone can carry): the copies' size (`resolution`:
// 0 = 720p, 1 = 1440p) and what NATIVE draws at (`draw_height`, 0 = the whole size).
struct Resolution {
  const char* name;
  int resolution, draw_height;
};
inline constexpr Resolution kResolutions[] = {
    {"360P", 0, 360}, {"480P", 0, 480}, {"540P", 0, 540}, {"720P", 0, 0}, {"1080P", 1, 1080}};
inline constexpr int kResolutionCount = int(std::size(kResolutions));
inline constexpr int kDefaultResolution = 3;  // 720p (the Android build's default since Session 63)

// frame_rate values as in gfx_settings.h: 1 60 everywhere, 0 classic (30 fps entrances), 2 high refresh (120 steps a second),
// 3 30 everywhere (Session 94 removed it, players asked for it back the same day: Jarrod, "rightfully claimed experimental").
struct FrameRate {
  const char* name;
  int value;
};
struct FrameRateChoice {
  const char* name;
  const char* note;
  int value;
};
inline constexpr FrameRateChoice kFrameRates[] = {
    {"STANDARD", "60 fps matches, 30 fps entrances", 0},
    {"60FPS ENTRANCES", "60 fps everywhere", 1},
    {"120FPS", "For stronger phones: the game runs at 120 fps while keeping real-time speed", 2},
    {"30FPS (EXPERIMENTAL)", "For weaker phones: 30 fps everywhere, matches included", 3}};

// The game's languages (launcher_core.h AutoLanguage: the runtime's user_language ids that change its text).
struct Language {
  const char* name;
  int id;
};
inline constexpr Language kLanguages[] = {{"AUTOMATIC", 0}, {"ENGLISH", 1}, {"GERMAN", 3},
                                          {"FRENCH", 4},    {"SPANISH", 5}, {"ITALIAN", 6}};

struct Settings {
  // The game files: Android - the content:// address the file picker gave (the app keeps read access to it); the PC
  // test build - a path. Empty = look in <files>/game (copied over USB).
  std::string game_uri;
  std::string game_name;  // what the picker called it (shown)
  int resolution = kDefaultResolution;  // index into kResolutions
  int frame_rate = 1;
  int texture_filter = 0;  // 0 standard, 1 8x, 2 16x
  int aspect = 0;          // 0 16:9 with bars (the game's own), 1 fill the screen (Hor+, src\ultrawide.h)
  int language = 0;        // kLanguages id; 0 = automatic
  bool dlc = true;
  bool superstars = false;  // Session 98: USE MY SUPERSTARS (the Superstar Importer's pack in <files>/mods/superstars; the PC launcher's key)
  std::string dlc_off;     // switched-off pack ids, "3,5"
  // Session 85 (Jarrod: "same work let's get it into android"): as the PC launcher's SHADOWS, CROWD DENSITY and DEPTH OF
  // FIELD (gfx_settings.h): defaults = the normal look; the SDK cvars gpu_dynamic_shadows, gpu_scene_crowd_density (0-100,
  // 10% steps) and gpu_depth_of_field.
  bool shadows = true;
  bool depth_of_field = true;
  int crowd_density = 100;
  // Session 93 (Jarrod: the low-end tier is chosen automatically from the GPU class, with a launcher override): 0 AUTO,
  // 1 LOW, 2 STANDARD (gfx_settings.h ResolveQualityTier). AUTO is LOW on every phone for now (refine later with the
  // SDK's `gpu_class` cvar, set from its quirks table at device creation). Session 95: LOW only supplies a STARTING value, the
  // crowd at 70% (ApplyLowStart, once); the sliders are what runs. A missing key = AUTO (no settings_version bump).
  int quality_tier = 0;
  // Session 95 (Jarrod, 2026-11-06: "The individual settings take priority but yes they start with our safe low defaults"):
  // LOW no longer lowers the crowd behind the slider. The first launch with this build (a first run, or a file from before the
  // key) writes LOW's starting crowd (70%) into the slider once (ApplyLowStart; mobile_main.cpp Start) and saves this flag.
  // After that the slider is the player's.
  bool tier_defaults_applied = false;
  // Session 90 (Jarrod: "put the true native one on my phone"): the PC launcher's Advanced TRUE NATIVE
  // (--wwe2k14_d3d_owned, src\d3d_owned.h). Session 93 (Jarrod, 2026-11-06 01:1x: "True native should become the
  // default, even retroactively ... we'll still have the toggle"): on by default; a file from before settings_version 1
  // moves to it once (LoadSettings), so an OFF picked after that stays.
  bool true_native = true;
  // Session 95 after v0.3.0 (Jarrod: "We should disable it, and honestly remove that whole storage bit from the launcher settings
  // now that it doesn't matter"): the Session 93 tester options are gone - the storage test (real vertex buffers removed the
  // 128 MiB dependence) and reduced precision (never measured on its own, the lead for white arena surfaces on Adreno 720 / 830).
  // Old files keep their "storage_test" / "mediump" keys; nothing reads them.
  int settings_version = 1;  // Session 93: 1 = the file has had the one-time move to TRUE NATIVE
  // Touch controls (ANDROID_PLAN F1, Jarrod 2026-10-28): 0 automatic (hidden while a controller is connected),
  // 1 always, 2 off; size and opacity in percent; a short vibration on each press (off by default).
  int touch_mode = 0;
  int touch_size = 100;
  int touch_opacity = 70;
  bool touch_vibrate = false;
  bool check_updates = true;
  // Session 73: dynamic resolution (src\dynamic_res.h): a step smaller while the phone cannot keep up.
  bool dynamic_res = true;
  // Session 94 (Jarrod): PERFORMANCE MODE, SAFE SHADERS and BIG CORES are gone (old files still load; the keys are ignored).
  // Session 73 (Jarrod's ideas 1, 2, 4 of the fourth ten): Snapdragon Game Super Resolution 1 from the game's picture
  // to the screen (SDK present_effect=sgsr) instead of a plain stretch; PERFORMANCE MODE (the scene drawn at 540p at
  // most + SGSR + the crowd members out of view left out, gpu_scene_crowd_cull 2, exact); FAST SHADERS (the pixel
  // shaders' colour maths at half precision, gpu_native_ps_relaxed_precision; a test).
  bool sgsr = true;
  bool fast_shaders = false;
  // Session 80 (Jarrod: "a little fps/frametime/temperature graph ... the toggle in the logging settings"; then "maybe we
  // could have both as options, light and verbose"): the game's performance overlay (src\perf_overlay.h): 0 off,
  // 1 light (the numbers only), 2 verbose (a box with the numbers and a frame-time graph).
  int perf_overlay = 0;
  // Session 72 (Android drivers, src\android_driver.h): a bundled custom Vulkan driver's file name, or (part 8) one the
  // player imported: kUserDriverPrefix + its folder under platform::DriversDir(); empty = the phone's own driver.
  std::string driver;
  // Session 73 (Jarrod's idea 10 of the fourth ten, a tester's Turnip 26.2.5 drew white surfaces and orange ropes): the
  // custom drivers the player said looked right / looked broken on this phone (comma-separated ids), asked once after
  // the first game that got going with one.
  std::string drivers_ok, drivers_bad;
};

inline bool IdListHas(const std::string& list, const std::string& id) {
  return !id.empty() && ("," + list + ",").find("," + id + ",") != std::string::npos;
}
inline void IdListAdd(std::string& list, const std::string& id) {
  if (id.empty() || IdListHas(list, id)) return;
  list += (list.empty() ? "" : ",") + id;
}

// Session 72 (Jarrod: "we should have many drivers for all the most common android devices"): the custom drivers
// the APK can carry, built from source by tools\android\drivers\build_drivers.ps1. The launcher offers the ones it
// finds in the app's library folder.
struct BundledDriver {
  const char* file;  // in the app's native library folder
  const char* name;  // shown
  const char* note;  // which phones
};
inline constexpr BundledDriver kDrivers[] = {
    // Session 73: + upstream Mesa's Adreno 710 / 720 / 722 entries (tools\android\drivers\mesa_patches).
    {"libvulkan_freedreno.so", "TURNIP (Mesa 26.2.5)",
     "Snapdragon phones: Adreno 6xx, 7xx (now also 710, 720 and 722) and 8xx."},
    {"libvulkan_panfrost.so", "PANVK (Mesa 27, experimental)",
     "Exynos, MediaTek and Tensor phones with a Mali GPU (G52 to G720), Android 12 or newer."},
};

inline constexpr const char* kUserDriverPrefix = "user:";

template <class T>
inline void Get(const nlohmann::json& j, const char* k, T& v) {
  if (j.contains(k)) try { v = j.at(k).get<T>(); } catch (...) {}
}

inline int ResolutionIndex(int resolution, int draw_height) {
  for (int i = 0; i < kResolutionCount; ++i)
    if (kResolutions[i].resolution == resolution && kResolutions[i].draw_height == draw_height) return i;
  return kDefaultResolution;
}

inline Settings LoadSettings(const fs::path& p) {
  Settings s;
  std::ifstream f(p, std::ios::binary);
  if (!f) return s;
  try {
    const nlohmann::json j = nlohmann::json::parse(f);
    int resolution = 0, draw_height = 0;
    Get(j, "game_uri", s.game_uri), Get(j, "game_name", s.game_name);
    Get(j, "resolution", resolution), Get(j, "draw_height", draw_height);
    s.resolution = ResolutionIndex(resolution, draw_height);
    Get(j, "frame_rate", s.frame_rate), Get(j, "texture_filter", s.texture_filter), Get(j, "aspect", s.aspect);
    Get(j, "language", s.language), Get(j, "dlc", s.dlc), Get(j, "dlc_off", s.dlc_off);
    Get(j, "superstars", s.superstars);  // Session 98
    Get(j, "touch_mode", s.touch_mode), Get(j, "touch_size", s.touch_size);
    Get(j, "touch_opacity", s.touch_opacity), Get(j, "touch_vibrate", s.touch_vibrate);
    Get(j, "check_updates", s.check_updates);
    Get(j, "driver", s.driver);
    Get(j, "drivers_ok", s.drivers_ok), Get(j, "drivers_bad", s.drivers_bad);
    Get(j, "dynamic_res", s.dynamic_res);
    Get(j, "sgsr", s.sgsr), Get(j, "fast_shaders", s.fast_shaders);
    Get(j, "perf_overlay", s.perf_overlay);
    Get(j, "shadows", s.shadows), Get(j, "depth_of_field", s.depth_of_field), Get(j, "crowd_density", s.crowd_density);
    Get(j, "true_native", s.true_native);  // Session 90
    Get(j, "quality_tier", s.quality_tier);  // Session 93
    Get(j, "tier_defaults_applied", s.tier_defaults_applied);
    int version = 0;  // Session 93: files from before it move to TRUE NATIVE once (the next save writes version 1)
    Get(j, "settings_version", version);
    if (version < 1) s.true_native = true;
  } catch (...) {
  }
  s.crowd_density = (std::clamp(s.crowd_density, 0, 100) + 5) / 10 * 10;  // Session 85
  if (s.frame_rate < 0 || s.frame_rate > 3) s.frame_rate = 1;
  s.texture_filter = std::clamp(s.texture_filter, 0, 2);
  s.quality_tier = std::clamp(s.quality_tier, 0, 2);
  s.aspect = std::clamp(s.aspect, 0, 1);
  s.touch_mode = std::clamp(s.touch_mode, 0, 2);
  s.touch_size = std::clamp(s.touch_size, 60, 150);
  s.touch_opacity = std::clamp(s.touch_opacity, 20, 100);
  s.perf_overlay = std::clamp(s.perf_overlay, 0, 2);
  return s;
}

// Session 95: LOW's player-visible starting value, written into the slider (first run on a phone, or the player picking LOW in
// GRAPHICS): the crowd 100% -> 70%. The resolution is left alone (the phone's default is already 720p; 1080P is a hand pick).
// Returns true when the crowd moved.
inline constexpr int kLowStartCrowd = 70;
inline bool ApplyLowStart(Settings& s) {
  if (s.crowd_density < 100) return false;
  s.crowd_density = kLowStartCrowd;
  return true;
}

inline bool SaveSettings(const fs::path& p, const Settings& s) {
  const Resolution& r = kResolutions[std::clamp(s.resolution, 0, kResolutionCount - 1)];
  const nlohmann::json j = {{"game_uri", s.game_uri},           {"game_name", s.game_name},
                            {"resolution", r.resolution},       {"draw_height", r.draw_height},
                            {"frame_rate", s.frame_rate},       {"texture_filter", s.texture_filter},
                            {"aspect", s.aspect},               {"language", s.language},
                            {"dlc", s.dlc},                     {"dlc_off", s.dlc_off},
                            {"superstars", s.superstars},
                            {"touch_mode", s.touch_mode},       {"touch_size", s.touch_size},
                            {"touch_opacity", s.touch_opacity}, {"touch_vibrate", s.touch_vibrate},
                            {"check_updates", s.check_updates}, {"driver", s.driver},
                            {"dynamic_res", s.dynamic_res},     {"sgsr", s.sgsr},
                            {"fast_shaders", s.fast_shaders},
                            {"drivers_ok", s.drivers_ok},       {"drivers_bad", s.drivers_bad},
                            {"perf_overlay", s.perf_overlay},   {"shadows", s.shadows},
                            {"depth_of_field", s.depth_of_field}, {"crowd_density", s.crowd_density},
                            {"quality_tier", s.quality_tier},       {"tier_defaults_applied", s.tier_defaults_applied},
                            {"true_native", s.true_native},     {"settings_version", s.settings_version}};
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2);
    if (!f) return false;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}

// The ids in "3,5" (dlc_catalog.h ParseIdList does the same for the game; this copy keeps the header free of it).
inline std::vector<int> ParseIds(const std::string& list) {
  std::vector<int> ids;
  std::stringstream ss(list);
  for (std::string t; std::getline(ss, t, ',');)
    if (!t.empty() && t.find_first_not_of("0123456789") == std::string::npos && t.size() < 6) ids.push_back(std::stoi(t));
  return ids;
}
inline std::string JoinIds(const std::vector<int>& ids) {
  std::string out;
  for (int id : ids) out += (out.empty() ? "" : ",") + std::to_string(id);
  return out;
}

// What the game gets besides the settings.
struct LaunchContext {
  std::string game_path;  // --game_data_root: a path, or kGameFdToken (the game's activity opens game_uri itself)
  std::string dlc_dir;    // the DLC folder (<files>/dlc); empty = none
  std::string mods_dir;   // Session 98: <files>/mods (the Superstar Importer's pack); empty = none
  int screen_w = 0, screen_h = 0;  // the screen in landscape (for "fill the screen")
  int wm30 = -1;          // a 30 Years of WrestleMania match to jump into (this launch only)
  std::string driver_trial;  // Session 72: the trial file for a custom driver (the game deletes it once running)
  // Session 72 part 8: an imported driver - its library's file name and its folder (with a trailing /).
  std::string driver_library, driver_dir;
  // Session 95 (LADDER): the SAFE MODE step in force (<files>/rung.txt, ladder.h); 0 = normal. Steps 1-2 are arguments only (not in the precompile key).
  int rung = 0;
  std::string boot_trial;  // Session 95 (LADDER): a file the game removes after 600 frames (or on the player's quit); left behind = it never got going
};
// Replaced by GameActivity with /proc/self/fd/N for the file it opened from game_uri (the open file stays with the
// game's process; the launcher's process cannot hand it over).
inline constexpr const char* kGameFdToken = "@GAME_FD@";

// What the scene is drawn at (0 = the copies' whole size). GameArgs and the precompile key both use it.
inline int EffectiveDrawHeight(const Settings& s) {
  const Resolution& r = kResolutions[std::clamp(s.resolution, 0, kResolutionCount - 1)];
  return r.draw_height;
}

// Session 95 (LADDER): SAFE MODE steps 2 and up lower these settings for this launch only (the saved settings stay): shadows off, crowd 30%, 30 FPS.
// None of them is in PrecompileKey, so no step 1-2 launch starts a precompile. There is no step 3 since Session 95 (it only turned reduced precision off).
inline Settings ApplyRung(Settings s, int rung) {
  if (rung >= 2) {
    s.shadows = false;
    s.crowd_density = std::min(s.crowd_density, 30);
    s.frame_rate = 3;
  }
  return s;
}

inline std::vector<std::string> GameArgs(const Settings& saved, const LaunchContext& c) {
  const Settings s = ApplyRung(saved, c.rung);
  std::vector<std::string> f;
  if (c.rung >= 1) {  // Session 95 (LADDER): the SDK's plain mode (gpu_quirks_apply.cpp) and one pipeline worker
    f.push_back("--gpu_plain_mode=on");
    f.push_back("--gpu_native_pipeline_workers=1");
  }
  if (!c.game_path.empty()) f.push_back("--game_data_root=" + c.game_path);
  // Graphics (gfx_settings.h GraphicsFlags with the NATIVE renderer, no upscaler, the phone's choices).
  const Resolution& r = kResolutions[std::clamp(s.resolution, 0, kResolutionCount - 1)];
  // Session 93: the quality tier (every phone is a phone: AUTO = LOW). Session 95: the tier changes no flag here; LOW's starting
  // crowd (70%) is written into the slider (ApplyLowStart), and the resolution is left alone (the default is 720p, 1080P a hand pick).
  // (gfx_settings.h ResolveQualityTier for a phone: LOW unless STANDARD is chosen; that header is not included here.)
  const bool low = s.quality_tier != 2;
  f.push_back(std::string("--wwe2k14_quality_note=") +
              (low ? (s.quality_tier == 1 ? "LOW_chosen" : "LOW_auto_phone") : "STANDARD_chosen"));
  if (r.resolution == 1) f.push_back("--resolution_scale=2");  // the Android build defaults to 1 (720p)
  const int draw_height = EffectiveDrawHeight(s);
  if (draw_height) f.push_back("--gpu_native_truth_draw_height=" + std::to_string(draw_height));
  if (s.sgsr) f.push_back("--present_effect=sgsr");
  // Session 85: the PC launcher's SHADOWS / CROWD DENSITY / DEPTH OF FIELD (flags only when not the default).
  if (!s.shadows) f.push_back("--gpu_dynamic_shadows=false");
  if (!s.depth_of_field) f.push_back("--gpu_depth_of_field=false");
  int crowd_pct = (std::clamp(s.crowd_density, 0, 100) + 5) / 10 * 10;  // (Session 95: the slider as it is; LOW's 70% is written into it, ApplyLowStart)
  if (const int crowd = crowd_pct; crowd < 100)
    f.push_back("--gpu_scene_crowd_density=" + std::to_string(crowd));
  // Session 90: TRUE NATIVE (the PC launcher's Advanced toggle; off = no flag).
  if (s.true_native) f.push_back("--wwe2k14_d3d_owned=true");
  // Session 93 (Jarrod, 2026-11-06 07:xx, after testing on his phone: "tested well ... I did see performance increases ...
  // remove the toggle and keep these changes as just straight up the default"): the phone defaults - BC1 -> ASTC textures
  // (only on GPUs without BC), [mediump colour maths: removed after v0.3.0], display-timed presents, the thermal
  // step-down (with DYNAMIC RES), the TRUE NATIVE pace gate (only under TRUE NATIVE).
  f.push_back("--vulkan_texture_transcode=true");
  // Session 95 (PLAIN): present_display_timing and the pace gate are no longer passed here: a command-line value outranks the SDK's
  // plain mode (gpu_plain_mode), which has to be able to turn both off on an unknown GPU. They are Android defaults in
  // src/android_pace.h (Apply), same values as before.
  f.push_back("--wwe2k14_android_thermal=true");
  // Session 73: Fast shaders off for now (on Jarrod's Adreno 830 the title and the match went black, only the ring's
  // banner left: half precision breaks values the PC's driver never runs at 16 bits). The setting stays, unused.
  // Session 73 (static in the background with fill-the-screen, Jarrod's phone only): truth passes keep colour with
  // LOAD_OP_NONE (gpu_native_truth_color_none, part 3) were off on phones while it was the suspect - areas a frame does
  // not redraw (fill-the-screen's sides) showed noise. Session 80: its bug found and fixed (one-draw passes were given
  // LOAD_OP_NONE / STORE_OP_NONE); phone run 164 with it on showed no white patches or flashes (Jarrod: "you can rebuild
  // the APK safely with it on") - no longer passed here: the game's own Android default (on) applies.
  // Session 73 (a 221 ms frame between Jarrod's two entrances, during ~5,500 failed pool-size probes - 357 MB asked
  // for in a 3.5 MB range - each a search of the physical heap's 131,072 pages on a phone CPU): Session 15's fast-fail
  // for requests bigger than their whole range (the same result, no search; its checks passed then).
  f.push_back("--wwe2k14_phys_alloc_fast_fail=true");
  // Session 73, b77: GPU busy work through loads, so each entrance does not start at the lowest GPU clock.
  f.push_back("--wwe2k14_gpu_keep_warm=true");
  // Session 95 (Lane E): the character read-back window follows the load, with the 300 frames as a floor (it can only get longer; src/create_readback.h:163-179). Also widens load_pipeline_wait.h InCharacterWindow. Savage on a phone.
  f.push_back("--wwe2k14_readback_char_window_by_load=true");
  if (s.frame_rate == 0 || s.frame_rate == 3) f.push_back("--wwe2k14_entrance_60fps=false");
  if (s.frame_rate == 3) {  // gfx_settings.h GraphicsFlags frame_rate 3: the game's own 30 fps mode, entrances keep their stock 30
    f.push_back("--wwe2k14_step_hz=30");
    f.push_back("--wwe2k14_step_hz_entrance=60");
    f.push_back("--vulkan_texture_load_direct_unscaled=true");
  }
  // Session 95 (F6): the decoupled 120 (gpu_d3d_decouple=9, word 0) is NOT mapped here: on phones it needs F3 (one frame boundary a tick, the pace gate) and the Adreno 830 multiview
  // check, neither built yet. FRAME RATE 2 stays 120 game steps a second.
  if (s.frame_rate == 2) {  // Session 94 (gfx_settings.h GraphicsFlags frame_rate 2 with 120 steps a second)
    f.push_back("--wwe2k14_step_hz=120");
    f.push_back("--wwe2k14_step_hz_entrance=120");
    f.push_back("--wwe2k14_step_vblank=true");
    f.push_back("--vulkan_allow_present_mode_immediate=false");  // mailbox: every frame shown, no tearing
  }
  if (s.texture_filter == 1 || s.texture_filter == 2)
    f.push_back(std::string("--wwe2k14_texture_filter=") + (s.texture_filter == 2 ? "16" : "8"));
  // Fill the screen: the phone's own shape (src\ultrawide.h takes W:H).
  if (s.aspect == 1 && c.screen_w > 0 && c.screen_h > 0)
    f.push_back("--wwe2k14_aspect=" + std::to_string(std::max(c.screen_w, c.screen_h)) + ":" +
                std::to_string(std::min(c.screen_w, c.screen_h)));
  if (s.language > 0) f.push_back("--user_language=" + std::to_string(s.language));
  if (s.dynamic_res) f.push_back("--wwe2k14_dynamic_res=true");  // Session 73
  if (s.perf_overlay) f.push_back("--wwe2k14_perf_overlay=" + std::to_string(s.perf_overlay));  // Session 80
  // DLC (launcher_core.h DlcFlags).
  if (s.dlc && !c.dlc_dir.empty()) {
    f.push_back("--wwe2k14_dlc=true");
    f.push_back("--wwe2k14_dlc_dir=" + c.dlc_dir);
    const std::string off = JoinIds(ParseIds(s.dlc_off));
    if (!off.empty()) f.push_back("--wwe2k14_dlc_off=" + off);
  }
  // Session 98 (Superstar Importer): USE MY SUPERSTARS (the game side, the flag's cvar and reading <mods>/superstars, is the importer core's)
  if (s.superstars && !c.mods_dir.empty()) {
    f.push_back("--wwe2k14_mods_superstars=true");
    f.push_back("--wwe2k14_mods_dir=" + c.mods_dir);
  }
  if (c.wm30 >= 0) f.push_back("--wwe2k14_wm30=" + std::to_string(c.wm30));
  // Session 72: a custom Vulkan driver (src\android_driver.h; GameActivity adds the library folder).
  // Part 8: an imported one from its own folder (the hooks stay in the library folder).
  if (!c.driver_dir.empty() && !c.driver_library.empty()) {
    f.push_back("--wwe2k14_android_driver=" + c.driver_library);
    f.push_back("--wwe2k14_android_driver_dir=" + c.driver_dir);
  } else if (!s.driver.empty() && s.driver.rfind(kUserDriverPrefix, 0) != 0) {
    f.push_back("--wwe2k14_android_driver=" + s.driver);
  }
  if (!s.driver.empty() && !c.driver_trial.empty()) f.push_back("--wwe2k14_android_driver_trial=" + c.driver_trial);
  if (!c.boot_trial.empty()) f.push_back("--wwe2k14_boot_trial=" + c.boot_trial);
  // The launcher's choices are the whole picture (launcher_core.h: the game must not apply a settings file on top).
  f.push_back("--wwe2k14_use_launcher_settings=false");
  return f;
}

// Session 93 (M6 step 2, the shader translation store; docs\native\d3d\S92_M6_DESIGN.md): the shader precompile only
// exists for TRUE NATIVE (the store is on under it, src\d3d_owned.h). The PC launcher's PrecompileKey, for the phone: the stamp
// (<files>/precompile_stamp.txt) holds this key after a precompile that finished, and the next PLAY runs the precompile again
// only when the key differs: a new app version (the GPU module's hash is the store's version and the shader list may have
// grown), another system build (the Vulkan driver), another GPU driver choice, and the settings that change the translator's
// configuration or the shaders' variants (the draw scale, TRUE NATIVE, FAST SHADERS,
// the texture filter). Everything else (touch, sound, crowd, shadows, ...) leaves the store valid.
inline bool WantsPrecompile(const Settings& s) { return s.true_native; }
inline std::string PrecompileKey(const Settings& s, const std::string& version, const std::string& device) {
  const Resolution& r = kResolutions[std::clamp(s.resolution, 0, kResolutionCount - 1)];
  return "app " + version + "; device " + device + "; driver " + s.driver + "; resolution " + std::to_string(r.resolution) +
         " draw_height " + std::to_string(EffectiveDrawHeight(s)) +
         " true_native " + std::to_string(int(s.true_native)) + " s93_phone_defaults 1 s95_phone_input 1" +  // (s95: the attribute forms join the store only in a precompile with vertex input on)
         " fast_shaders " + std::to_string(int(s.fast_shaders)) + " texture_filter " + std::to_string(s.texture_filter) +
         " mediump 0";  // (Session 95: full precision for every phone; was the default's empty suffix, so stores rebuild once)
}

}  // namespace mlauncher
