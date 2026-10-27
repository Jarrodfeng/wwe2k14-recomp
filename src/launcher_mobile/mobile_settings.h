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
};

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
                            {"check_updates", s.check_updates}};
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
  if (r.draw_height) f.push_back("--gpu_native_truth_draw_height=" + std::to_string(r.draw_height));
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
  // DLC (launcher_core.h DlcFlags).
  if (s.dlc && !c.dlc_dir.empty()) {
    f.push_back("--wwe2k14_dlc=true");
    f.push_back("--wwe2k14_dlc_dir=" + c.dlc_dir);
    const std::string off = JoinIds(ParseIds(s.dlc_off));
    if (!off.empty()) f.push_back("--wwe2k14_dlc_off=" + off);
  }
  if (c.wm30 >= 0) f.push_back("--wwe2k14_wm30=" + std::to_string(c.wm30));
  // The launcher's choices are the whole picture (launcher_core.h: the game must not apply a settings file on top).
  f.push_back("--wwe2k14_use_launcher_settings=false");
  return f;
}

}  // namespace mlauncher
