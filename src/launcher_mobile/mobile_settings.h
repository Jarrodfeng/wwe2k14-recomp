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

// frame_rate values as in gfx_settings.h: 1 60 everywhere, 0 classic (30 fps entrances), 3 30 everywhere.
struct FrameRate {
  const char* name;
  int value;
};
inline constexpr FrameRate kFrameRates[] = {{"60 FPS", 1}, {"60 MATCHES, 30 ENTRANCES", 0}, {"30 FPS (BATTERY)", 3}};

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
  std::string dlc_off;     // switched-off pack ids, "3,5"
  // Touch controls (ANDROID_PLAN F1, Jarrod 2026-10-28): 0 automatic (hidden while a controller is connected),
  // 1 always, 2 off; size and opacity in percent; a short vibration on each press (off by default).
  int touch_mode = 0;
  int touch_size = 100;
  int touch_opacity = 70;
  bool touch_vibrate = false;
  bool check_updates = true;
  // Session 73: the NATIVE renderer's shader shortcuts off (SafeShaderFlags), for a tester to tell whether a broken
  // picture comes from them.
  bool safe_shaders = false;
  // Session 73: dynamic resolution (src\dynamic_res.h): a step smaller while the phone cannot keep up.
  bool dynamic_res = true;
  // Session 73: the busy game threads kept off the slowest CPU cores (srcndroid_perf_hint.h); a test switch.
  bool big_cores = false;
  // Session 73 (Jarrod's ideas 1, 2, 4 of the fourth ten): Snapdragon Game Super Resolution 1 from the game's picture
  // to the screen (SDK present_effect=sgsr) instead of a plain stretch; PERFORMANCE MODE (the scene drawn at 540p at
  // most + SGSR + the crowd members out of view left out, gpu_scene_crowd_cull 2, exact); FAST SHADERS (the pixel
  // shaders' colour maths at half precision, gpu_native_ps_relaxed_precision; a test).
  bool sgsr = true;
  bool performance = false;
  bool fast_shaders = false;
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
    Get(j, "touch_mode", s.touch_mode), Get(j, "touch_size", s.touch_size);
    Get(j, "touch_opacity", s.touch_opacity), Get(j, "touch_vibrate", s.touch_vibrate);
    Get(j, "check_updates", s.check_updates);
    Get(j, "driver", s.driver);
    Get(j, "drivers_ok", s.drivers_ok), Get(j, "drivers_bad", s.drivers_bad);
    Get(j, "safe_shaders", s.safe_shaders);
    Get(j, "dynamic_res", s.dynamic_res);
    Get(j, "big_cores", s.big_cores);
    Get(j, "sgsr", s.sgsr), Get(j, "performance", s.performance), Get(j, "fast_shaders", s.fast_shaders);
  } catch (...) {
  }
  if (s.frame_rate != 0 && s.frame_rate != 1 && s.frame_rate != 3) s.frame_rate = 1;
  s.texture_filter = std::clamp(s.texture_filter, 0, 2);
  s.aspect = std::clamp(s.aspect, 0, 1);
  s.touch_mode = std::clamp(s.touch_mode, 0, 2);
  s.touch_size = std::clamp(s.touch_size, 60, 150);
  s.touch_opacity = std::clamp(s.touch_opacity, 20, 100);
  return s;
}

inline bool SaveSettings(const fs::path& p, const Settings& s) {
  const Resolution& r = kResolutions[std::clamp(s.resolution, 0, kResolutionCount - 1)];
  const nlohmann::json j = {{"game_uri", s.game_uri},           {"game_name", s.game_name},
                            {"resolution", r.resolution},       {"draw_height", r.draw_height},
                            {"frame_rate", s.frame_rate},       {"texture_filter", s.texture_filter},
                            {"aspect", s.aspect},               {"language", s.language},
                            {"dlc", s.dlc},                     {"dlc_off", s.dlc_off},
                            {"touch_mode", s.touch_mode},       {"touch_size", s.touch_size},
                            {"touch_opacity", s.touch_opacity}, {"touch_vibrate", s.touch_vibrate},
                            {"check_updates", s.check_updates}, {"driver", s.driver},
                            {"safe_shaders", s.safe_shaders},   {"dynamic_res", s.dynamic_res},
                            {"big_cores", s.big_cores},         {"sgsr", s.sgsr},
                            {"performance", s.performance},     {"fast_shaders", s.fast_shaders},
                            {"drivers_ok", s.drivers_ok},       {"drivers_bad", s.drivers_bad}};
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
  int screen_w = 0, screen_h = 0;  // the screen in landscape (for "fill the screen")
  int wm30 = -1;          // a 30 Years of WrestleMania match to jump into (this launch only)
  std::string driver_trial;  // Session 72: the trial file for a custom driver (the game deletes it once running)
  // Session 72 part 8: an imported driver - its library's file name and its folder (with a trailing /).
  std::string driver_library, driver_dir;
};
// Replaced by GameActivity with /proc/self/fd/N for the file it opened from game_uri (the open file stays with the
// game's process; the launcher's process cannot hand it over).
inline constexpr const char* kGameFdToken = "@GAME_FD@";

inline std::vector<std::string> GameArgs(const Settings& s, const LaunchContext& c) {
  std::vector<std::string> f;
  if (!c.game_path.empty()) f.push_back("--game_data_root=" + c.game_path);
  // Graphics (gfx_settings.h GraphicsFlags with the NATIVE renderer, no upscaler, the phone's choices).
  const Resolution& r = kResolutions[std::clamp(s.resolution, 0, kResolutionCount - 1)];
  if (r.resolution == 1) f.push_back("--resolution_scale=2");  // the Android build defaults to 1 (720p)
  // Session 73: PERFORMANCE MODE draws the scene at 540p at most (the copies stay at the chosen size).
  const int draw_height = s.performance && r.resolution == 0 && (r.draw_height == 0 || r.draw_height > 540)
                              ? 540
                              : r.draw_height;
  if (draw_height) f.push_back("--gpu_native_truth_draw_height=" + std::to_string(draw_height));
  if (s.sgsr || s.performance) f.push_back("--present_effect=sgsr");
  if (s.performance) f.push_back("--gpu_scene_crowd_cull=2");
  // Session 73: Fast shaders off for now (on Jarrod's Adreno 830 the title and the match went black, only the ring's
  // banner left: half precision breaks values the PC's driver never runs at 16 bits). The setting stays, unused.
  // Session 73 (static in the background with fill-the-screen, Jarrod's phone only): truth passes keep colour with
  // LOAD_OP_NONE (gpu_native_truth_color_none, part 3) off on phones while it is the suspect - areas a frame does not
  // redraw (fill-the-screen's sides) showed noise; never measured faster on a phone.
  f.push_back("--gpu_native_truth_color_none=false");
  // Session 73 (a 221 ms frame between Jarrod's two entrances, during ~5,500 failed pool-size probes - 357 MB asked
  // for in a 3.5 MB range - each a search of the physical heap's 131,072 pages on a phone CPU): Session 15's fast-fail
  // for requests bigger than their whole range (the same result, no search; its checks passed then).
  f.push_back("--wwe2k14_phys_alloc_fast_fail=true");
  // Session 73, b77: GPU busy work through loads, so each entrance does not start at the lowest GPU clock.
  f.push_back("--wwe2k14_gpu_keep_warm=true");
  if (s.frame_rate == 0 || s.frame_rate == 3) f.push_back("--wwe2k14_entrance_60fps=false");
  if (s.frame_rate == 3) {
    f.push_back("--wwe2k14_step_hz=30");
    f.push_back("--wwe2k14_step_hz_entrance=60");
    f.push_back("--vulkan_texture_load_direct_unscaled=true");
  }
  if (s.texture_filter == 1 || s.texture_filter == 2)
    f.push_back(std::string("--wwe2k14_texture_filter=") + (s.texture_filter == 2 ? "16" : "8"));
  // Fill the screen: the phone's own shape (src\ultrawide.h takes W:H).
  if (s.aspect == 1 && c.screen_w > 0 && c.screen_h > 0)
    f.push_back("--wwe2k14_aspect=" + std::to_string(std::max(c.screen_w, c.screen_h)) + ":" +
                std::to_string(std::min(c.screen_w, c.screen_h)));
  if (s.language > 0) f.push_back("--user_language=" + std::to_string(s.language));
  if (s.dynamic_res) f.push_back("--wwe2k14_dynamic_res=true");  // Session 73
  if (s.big_cores) f.push_back("--wwe2k14_android_big_cores=true");  // Session 73
  // Session 73: SAFE SHADERS - the shaders as the emulator translates them, without NATIVE's shortcuts (the Xbox 360's
  // multiply rule kept, no fused multiply-add, no baked-in endian / texture sign / epilogue variants). Only the 4090
  // and Jarrod's Adreno 830 have run the shortcuts; a phone's driver may build them wrongly.
  if (s.safe_shaders)
    for (const char* flag : {"--gpu_native_vs_mul_mode=0", "--gpu_native_ps_mul_mode=0", "--gpu_native_fma=0",
                             "--gpu_native_vs_mul_fetch_finite=false", "--gpu_native_vertex_endian_spec=false",
                             "--gpu_native_ps_texture_spec=false", "--gpu_native_ps_epilogue_spec=false",
                             "--gpu_native_ps_implicit_lod=false"})
      f.push_back(flag);
  // DLC (launcher_core.h DlcFlags).
  if (s.dlc && !c.dlc_dir.empty()) {
    f.push_back("--wwe2k14_dlc=true");
    f.push_back("--wwe2k14_dlc_dir=" + c.dlc_dir);
    const std::string off = JoinIds(ParseIds(s.dlc_off));
    if (!off.empty()) f.push_back("--wwe2k14_dlc_off=" + off);
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
  // The launcher's choices are the whole picture (launcher_core.h: the game must not apply a settings file on top).
  f.push_back("--wwe2k14_use_launcher_settings=false");
  return f;
}

}  // namespace mlauncher
