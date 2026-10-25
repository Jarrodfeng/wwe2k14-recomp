// wwe2k14 launcher - the logic behind the window (Session 17 item 1C). No UI code here.
//
// The launcher is a separate program next to wwe2k14.exe. It never changes the game's files: trons are assigned by
// copying the chosen video into the mods folder (<mods>\titantron\NNN.<ext>, the folder the game reads with
// --wwe2k14_mods=true, src\mods.h), and the Play tab only builds and runs a command line. Its own settings live in
// %APPDATA%\wwe2k14-launcher\settings.json (never in the repo).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "disc_image.h"
#include "../gfx_settings.h"  // Session 28: the graphics settings mapping the game shares
#include "../dlc_catalog.h"   // Session 32: DLC packs (finding them, reading their catalogs), shared with the game

namespace launcher {

namespace fs = std::filesystem;
using json = nlohmann::json;

inline std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
inline std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
inline std::string PathStr(const fs::path& p) { return Narrow(p.wstring()); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
inline std::string Upper(std::string s) {  // Session 31: names in the game's capitals (ASCII only)
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
inline std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---------------------------------------------------------------------------------------------------------------
// Settings (per user, %APPDATA%\wwe2k14-launcher\settings.json)

// Session 33: the Discord application the game's status is shown under (made by Jarrod on Discord's developer site;
// its name is what Discord shows as the game). Empty until he sets it: Settings -> Game can take one too.
inline constexpr const char* kDiscordAppId = "";

struct Settings {
  std::string game_exe;        // empty = wwe2k14.exe next to the launcher
  std::string mods_dir;        // empty = the game's default (mods next to the game data folder)
  std::string scenario;        // Play tab
  int wm30 = -1;               // Session 34: a 30 Years of WrestleMania match to jump into (this launch only)
  int preset = 0;              // Preset enum
  bool mods = true;
  bool skip_entrances = false;
  bool logo_legacy_route = false;  // Session 27: the logo import's Session 25 counted presses (no memory checks)
  // Session 27 item 8: Quick Match built from our id tables (0 = build, 1 = a saved setup).
  int quick_mode = 0, quick_type = 0, quick_arena = -1;
  std::string quick_ids = "343,324";
  bool quick_cpu_only = false;  // false: wrestler 1 on controller 1 (the player); true: everyone CPU
  bool cinecam = false;
  int look = 0;                // 0 = default (1440p 60), 1 = original (720p), 2 = 4K
  // Session 33: 5 FSR native, 6 FSR quality (1440p -> 4K), 7 FSR performance (720p -> 1440p), any card.
  // Session 24: 0 off, 1 DLAA, 2 DLSS performance (720p -> 1440p), 3 quality (1440p -> 4K), 4 ultra performance
  // (720p -> 4K); the NVIDIA runtime folder (empty = the files bundled with a release).
  int upscale = 0;
  // Session 37: why the game turned FSR off on this PC (src\upscale_guard.h writes it; shown under Graphics until the
  // player picks an upscaler again).
  std::string fsr_note;
  int renderer = 2;            // Session 37: 0 accurate, 1 light (weak / integrated GPUs; src\light_renderer.h);
                               // Session 41: 2 native (accurate + the native renderer, --gpu_native=2); Session 51
                               // (Jarrod, 2026-10-22: "NATIVE for everyone"): 2 by default
  int settings_version = wwe2k14::gfxs::kSettingsVersion;  // Session 51: the old default renderer moved to NATIVE (gfx_settings.h)
  int loaded_version = 0;  // Session 54: the file's settings_version as read (not saved; 0 = no file / none)
  int texture_filter = 0;      // Session 33: 0 standard (4x), 1 8x, 2 16x anisotropic filtering
  bool discord = false;        // Session 33: Discord status (off by default)
  int aspect = 0;              // Session 33: 0 = 16:9 (the game's own), 1 = auto (the monitor's: ultrawide Hor+)
  bool photo_mode = false;     // Session 33: photo mode (F10 / L3 + R3 in a match)
  std::string discord_app_id;  // the Discord application id (empty = kDiscordAppId below)
  std::string dlss_dir;
  bool arena_override = false;
  int arena = 0;
  std::string participants;    // --wwe2k14_launch_participants value
  std::string replay_name = "my_match";
  bool replay_record = true;   // Replay preset: record (else play)
  std::string replay_play;     // existing .rec name
  std::string reshoot_shots;   // comma list of CineCam path files
  std::string reshoot_anchor = "clock";
  int seed = 13;
  int sweep_from = 1, sweep_to = 10;
  std::string seed_filter;     // find.py style: winner=0 finish=1 max_clock=300
  std::string director;        // director script name (out\director\NAME.txt)
  bool director_seed = false;  // Director preset: use `seed` instead of the script's
  std::string extra;           // extra flags
  bool harness = false;        // run through tools\bench\run_list.ps1 (frozen save + run lock)
  std::string harness_route = "launch_boot_watch.txt";
  int window_w = 1280, window_h = 820;
  int trons_view = 0;          // Trons tab: 0 list, 1 grid
  int trons_sort = 0;          // 0 name, 1 id, 2 tron, 3 override
  bool trons_sort_desc = false;
  // Session 25: the launcher for players (sidebar: Play Game, Mods, Settings).
  std::string game_data_dir;   // default.xex's folder or a disc image (.iso, S26); empty = game_data\extracted
  std::string user_data_dir;   // the game's save folder (empty = Documents\wwe2k14, the game's default)
  bool first_run_done = false; // the first-run wizard has been finished
  int resolution = 1;          // 0 720p, 1 1440p (the game's default), 2 4K
  int draw_height = 0;         // Session 54: NATIVE draws at 360 / 480 / 540 (resolution 0) or 1080 (1); 0 = none
  int frame_rate = 1;          // 0 classic (30 fps entrances, 60 matches), 1 60 everywhere, 2 high refresh
  int refresh_hz = 0;          // high refresh: game steps per second (0 = the monitor's refresh rate)
  int perf_report = -1;        // write logs\perf_report_NNN.txt: 1 on, 0 off, -1 the build's default (playtest: on)
  bool original_look = false;  // the original 720p / 4x MSAA / 30 fps entrance look (Advanced)
  int page = 0;                // 0 Play, 1 Mods, 2 Settings, 3 About (Session 31)
  int mods_tab = 0;            // 0 Trons, 1 Titles, 2 Logos
  bool runin = false;          // run-ins: F9 in a match (Session 16 experiment)
  bool keyboard = false;       // ReXGlue's keyboard-as-controller driver (mnk_mode; untested in this project)
  // Session 31 (a playtester's suggestion): the game's language (0 = automatic: Windows' language when the disc has it,
  // else English; else the runtime's user_language id: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian), the
  // graphics card (empty = automatic: the game picks the dedicated GPU, src\gpu_pick.h; else part of its name), and
  // windowed instead of fullscreen.
  int language = 0;
  std::string gpu;
  bool windowed = false;
  int titles_view = 0;         // Mods -> Titles: 0 list, 1 grid (Session 31)
  bool check_updates = true;   // ask GitHub for a newer full release at start (Session 31, update_check.h)
  // Session 32: the player's DLC packs (src\dlc.h, src\dlc_catalog.h): on / off, the folder (empty = `dlc` next to the
  // game data folder, else next to the launcher) and the pack ids switched off ("3,5"). The game reads the same three
  // keys from this file when it is started without the launcher.
  bool dlc = true;
  std::string dlc_dir;
  std::string dlc_off;
  // Session 53 (a playtester: "make the launcher open the game automatic"; Jarrod: "a toggle in the game settings"):
  // the launcher starts the game at once when it opens (3 seconds to stay, Shift held skips it).
  bool quick_start = false;
  // Session 53 (a playtester: "an option to change the size of the logo when importing. 256x256 or 128x128"): the
  // canvas the logo import picks in the game's SELECT CANVAS SIZE menu.
  int logo_size = 256;
};

inline void to_json(json& j, const Settings& s) {
  j = json{{"game_exe", s.game_exe}, {"mods_dir", s.mods_dir}, {"scenario", s.scenario}, {"preset", s.preset},
           {"mods", s.mods}, {"skip_entrances", s.skip_entrances}, {"logo_legacy_route", s.logo_legacy_route}, {"quick_mode", s.quick_mode}, {"quick_type", s.quick_type}, {"quick_arena", s.quick_arena}, {"quick_ids", s.quick_ids}, {"quick_cpu_only", s.quick_cpu_only}, {"cinecam", s.cinecam}, {"look", s.look},
           {"upscale", s.upscale}, {"texture_filter", s.texture_filter}, {"dlss_dir", s.dlss_dir},
           {"fsr_note", s.fsr_note}, {"renderer", s.renderer}, {"settings_version", s.settings_version},
           {"discord", s.discord}, {"discord_app_id", s.discord_app_id}, {"aspect", s.aspect},
           {"photo_mode", s.photo_mode},
           {"arena_override", s.arena_override}, {"arena", s.arena}, {"participants", s.participants},
           {"replay_name", s.replay_name}, {"replay_record", s.replay_record}, {"replay_play", s.replay_play},
           {"reshoot_shots", s.reshoot_shots}, {"reshoot_anchor", s.reshoot_anchor}, {"seed", s.seed},
           {"sweep_from", s.sweep_from}, {"sweep_to", s.sweep_to}, {"seed_filter", s.seed_filter},
           {"director", s.director}, {"director_seed", s.director_seed}, {"extra", s.extra}, {"harness", s.harness}, {"harness_route", s.harness_route},
           {"window_w", s.window_w}, {"window_h", s.window_h}, {"trons_view", s.trons_view},
           {"trons_sort", s.trons_sort}, {"trons_sort_desc", s.trons_sort_desc},
           {"game_data_dir", s.game_data_dir}, {"user_data_dir", s.user_data_dir}, {"first_run_done", s.first_run_done},
           {"resolution", s.resolution}, {"draw_height", s.draw_height}, {"frame_rate", s.frame_rate},
           {"refresh_hz", s.refresh_hz},
           {"perf_report", s.perf_report}, {"original_look", s.original_look}, {"page", s.page},
           {"mods_tab", s.mods_tab}, {"runin", s.runin}, {"keyboard", s.keyboard},
           {"language", s.language}, {"gpu", s.gpu}, {"windowed", s.windowed}, {"titles_view", s.titles_view}, {"check_updates", s.check_updates},
           {"dlc", s.dlc}, {"dlc_dir", s.dlc_dir}, {"dlc_off", s.dlc_off}, {"quick_start", s.quick_start}, {"logo_size", s.logo_size}};
}
template <class T>
inline void Get(const json& j, const char* k, T& v) {
  if (j.contains(k)) try { v = j.at(k).get<T>(); } catch (...) {}
}
inline void from_json(const json& j, Settings& s) {
  Get(j, "game_exe", s.game_exe), Get(j, "mods_dir", s.mods_dir), Get(j, "scenario", s.scenario);
  Get(j, "preset", s.preset), Get(j, "mods", s.mods), Get(j, "skip_entrances", s.skip_entrances);
  Get(j, "logo_legacy_route", s.logo_legacy_route);
  Get(j, "quick_mode", s.quick_mode), Get(j, "quick_type", s.quick_type), Get(j, "quick_arena", s.quick_arena);
  Get(j, "quick_ids", s.quick_ids), Get(j, "quick_cpu_only", s.quick_cpu_only);
  Get(j, "cinecam", s.cinecam), Get(j, "look", s.look), Get(j, "arena_override", s.arena_override);
  Get(j, "upscale", s.upscale), Get(j, "texture_filter", s.texture_filter), Get(j, "dlss_dir", s.dlss_dir);
  Get(j, "fsr_note", s.fsr_note), Get(j, "renderer", s.renderer);
  // Session 51 (Jarrod, 2026-10-22: "NATIVE for everyone"): a file from before it with the old default renderer
  // (ACCURATE) moves to NATIVE once; the next save writes the version, so a renderer picked after that stays.
  int version = 0;
  Get(j, "settings_version", version);
  if (version < 2 && s.renderer == 0) s.renderer = 2;
  // Session 61 (Jarrod: "remove the renderer option entirely, with light and accurate as advanced flags only"): the
  // stored renderer is no choice any more - always NATIVE (Extra flags --wwe2k14_renderer=light|accurate, BuildFlags).
  s.renderer = 2;
  s.loaded_version = version;  // Session 54 (the launcher's device moves, MigrateForDevice)
  s.settings_version = wwe2k14::gfxs::kSettingsVersion;
  Get(j, "discord", s.discord), Get(j, "discord_app_id", s.discord_app_id), Get(j, "aspect", s.aspect);
  Get(j, "photo_mode", s.photo_mode);
  Get(j, "arena", s.arena), Get(j, "participants", s.participants), Get(j, "replay_name", s.replay_name);
  Get(j, "replay_record", s.replay_record), Get(j, "replay_play", s.replay_play);
  Get(j, "reshoot_shots", s.reshoot_shots), Get(j, "reshoot_anchor", s.reshoot_anchor), Get(j, "seed", s.seed);
  Get(j, "sweep_from", s.sweep_from), Get(j, "sweep_to", s.sweep_to), Get(j, "seed_filter", s.seed_filter);
  Get(j, "director", s.director), Get(j, "director_seed", s.director_seed), Get(j, "extra", s.extra), Get(j, "harness", s.harness);
  Get(j, "harness_route", s.harness_route), Get(j, "window_w", s.window_w), Get(j, "window_h", s.window_h);
  Get(j, "trons_view", s.trons_view), Get(j, "trons_sort", s.trons_sort), Get(j, "trons_sort_desc", s.trons_sort_desc);
  Get(j, "game_data_dir", s.game_data_dir), Get(j, "user_data_dir", s.user_data_dir);
  Get(j, "first_run_done", s.first_run_done), Get(j, "resolution", s.resolution), Get(j, "frame_rate", s.frame_rate);
  Get(j, "refresh_hz", s.refresh_hz), Get(j, "perf_report", s.perf_report), Get(j, "original_look", s.original_look);
  Get(j, "page", s.page), Get(j, "mods_tab", s.mods_tab), Get(j, "runin", s.runin), Get(j, "keyboard", s.keyboard);
  Get(j, "language", s.language), Get(j, "gpu", s.gpu), Get(j, "windowed", s.windowed), Get(j, "titles_view", s.titles_view), Get(j, "check_updates", s.check_updates);
  Get(j, "dlc", s.dlc), Get(j, "dlc_dir", s.dlc_dir), Get(j, "dlc_off", s.dlc_off);
  Get(j, "quick_start", s.quick_start), Get(j, "logo_size", s.logo_size);
  Get(j, "draw_height", s.draw_height);  // Session 54
  s.resolution = std::clamp(s.resolution, 0, 2);
  s.draw_height = wwe2k14::gfxs::ValidDrawHeight(s.resolution, s.draw_height);
  if (s.logo_size != 128) s.logo_size = 256;
  if (!j.contains("resolution") && j.contains("look")) {  // the Session 17-24 Play tab's "Look"
    if (s.look == 1) s.original_look = true;
    if (s.look == 2) s.resolution = 2;
  }
}

inline fs::path SettingsPath() {
  PWSTR appdata = nullptr;
  fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) dir = appdata;
  CoTaskMemFree(appdata);
  if (dir.empty()) dir = fs::temp_directory_path();
  return dir / "wwe2k14-launcher" / "settings.json";
}

// The launcher's own data on this PC: %LOCALAPPDATA%\wwe2k14-launcher (previews, backups, a release's scenarios and
// recordings). Made from the user's own game files, so it stays on this PC (never in the repo or a release).
inline fs::path g_local_data_override;  // Session 53: --local-data DIR (tests: never the player's own folder)
inline fs::path LocalDataDir() {
  if (!g_local_data_override.empty()) return g_local_data_override;
  PWSTR local = nullptr;
  fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
  CoTaskMemFree(local);
  if (dir.empty()) dir = fs::temp_directory_path();
  return dir / "wwe2k14-launcher";
}

// Tron thumbnails / previews exported by the game ("Build previews").
inline fs::path TronCacheDir() { return LocalDataDir() / "trons"; }

// The game's user data (save) folder when no --user_data_root is given: ReXGlue's default, Documents\wwe2k14
// (rex::filesystem::GetUserFolder() + the app name).
inline fs::path DefaultUserRoot() {
  PWSTR docs = nullptr;
  fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) dir = docs;
  CoTaskMemFree(docs);
  return dir / "wwe2k14";
}

inline Settings LoadSettings(const fs::path& p) {
  Settings s;
  std::ifstream f(p);
  if (f) try { s = json::parse(f).get<Settings>(); } catch (...) {}
  return s;
}

inline void SaveSettings(const fs::path& p, const Settings& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::trunc);
  if (f) f << json(s).dump(2);
}

// ---------------------------------------------------------------------------------------------------------------
// Where things are. The dev layout (this repo): the launcher sits in out\build\<preset>\ next to wwe2k14.exe, and the
// repo root is the first folder above it that has game_data\extracted\default.xex (the game searches the same way).
// A release (Session 25): the launcher and the game in one folder, our id tables in data\ids, the game files wherever
// the player keeps them (Settings -> Game data), the player's scenarios and recordings in LocalDataDir().

struct Paths {
  fs::path launcher_dir, repo, game_exe, game_data, mods, ids, scenarios, replays, director, seeds, shots, routes;
  fs::path dlc;        // Session 32: the DLC folder (it may not exist)
  fs::path user_data;  // the game's save folder
  fs::path logs;       // the game's logs (next to the game exe)
  fs::path nvidia;     // a release's bundled NVIDIA DLSS files (nvidia\sl.interposer.dll next to the launcher), or empty
  bool nvidia_bundled = false;  // Session 31: nvidia is the release's folder (else the repo's Streamline SDK, dev builds)
  bool repo_found = false;
};

// The monitor's refresh rate (the App fills it in at start; used by the "High refresh" frame rate).
inline int g_monitor_hz = 60;
// The performance report's default when the player hasn't chosen (playtest builds: on; the App sets it).
inline bool g_perf_report_default = false;

// A folder with NVIDIA's Streamline runtime (sl.interposer.dll) in it.
inline bool HasStreamline(const std::string& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(fs::path(Widen(dir)) / "sl.interposer.dll", ec);
}

inline Paths FindPaths(const Settings& s) {
  Paths p;
  wchar_t buf[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  p.launcher_dir = fs::path(buf).parent_path();
  std::error_code ec;
  for (fs::path d = p.launcher_dir; !d.empty(); d = d.parent_path()) {
    if (fs::exists(d / "game_data" / "extracted" / "default.xex", ec)) {
      p.repo = d, p.repo_found = true;
      break;
    }
    if (d == d.parent_path()) break;
  }
  if (!p.repo_found) p.repo = p.launcher_dir;
  p.game_exe = s.game_exe.empty() ? p.launcher_dir / "wwe2k14.exe" : fs::path(Widen(s.game_exe));
  p.logs = p.game_exe.parent_path() / "logs";
  {
    std::error_code ec;
    if (fs::is_regular_file(p.launcher_dir / "nvidia" / "sl.interposer.dll", ec)) {
      p.nvidia = p.launcher_dir / "nvidia", p.nvidia_bundled = true;
    } else if (p.repo_found) {
      // Session 31: a development build uses the Streamline SDK the release bundles its files from.
      const fs::path sdk = p.repo / "thirdparty" / "streamline" / "sdk" / "bin" / "x64";
      if (fs::is_regular_file(sdk / "sl.interposer.dll", ec)) p.nvidia = sdk;
    }
  }
  if (!s.game_data_dir.empty()) p.game_data = fs::path(Widen(s.game_data_dir));
  else if (p.repo_found) p.game_data = p.repo / "game_data" / "extracted";
  p.user_data = s.user_data_dir.empty() ? DefaultUserRoot() : fs::path(Widen(s.user_data_dir));
  // Session 32: the game's own rule (dlc_catalog.h), so a game started without the launcher finds the same folder.
  p.dlc = s.dlc_dir.empty() ? wwe2k14::dlc::DefaultDir(p.game_data, p.game_exe.parent_path()) : fs::path(Widen(s.dlc_dir));
  if (p.repo_found) {
    p.mods = s.mods_dir.empty() ? p.game_data.parent_path() / "mods" : fs::path(Widen(s.mods_dir));
    p.ids = p.repo / "db" / "ids";
    p.scenarios = p.repo / "out" / "scenarios";
    p.replays = p.repo / "out" / "replays";
    p.director = p.repo / "out" / "director";
    p.seeds = p.repo / "out" / "seeds";
    p.shots = p.repo / "out" / "harness" / "cinecam" / "paths";
    p.routes = p.repo / "tools" / "input_scripts";
  } else {
    const fs::path mine = LocalDataDir();
    p.mods = s.mods_dir.empty() ? p.launcher_dir / "mods" : fs::path(Widen(s.mods_dir));
    p.ids = p.launcher_dir / "data" / "ids";
    p.scenarios = mine / "scenarios";
    p.replays = mine / "replays";
    p.director = mine / "director";
    p.seeds = mine / "seeds";
    p.shots = mine / "cinecam_paths";
    p.routes = p.launcher_dir / "data" / "routes";
  }
  return p;
}

// ---------------------------------------------------------------------------------------------------------------
// Characters and their trons (db\ids\characters.json: our own id table)

struct Character {
  int id = -1;
  std::string name, kind;
  int tron = -1;      // the titantron file number this character's entrance plays
  std::string note;   // team / era note
  int dlc_pack = -1;  // Session 32: a DLC wrestler's pack id (-1 = on the disc); listed only while that pack is on
};

struct Arena {
  int id = -1;
  std::string name;
};

inline std::vector<Character> LoadCharacters(const fs::path& ids_dir, std::string& err) {
  std::vector<Character> out;
  std::ifstream f(ids_dir / "characters.json");
  if (!f) {
    err = "can't read " + PathStr(ids_dir / "characters.json");
    return out;
  }
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    err = e.what();
    return out;
  }
  std::map<int, std::string> team_of_tron;
  for (const auto& t : j.value("team_trons", json::array())) {
    if (t.contains("tron")) team_of_tron[t["tron"].get<int>()] = t.value("team", "");
  }
  for (const auto& e : j.value("entries", json::array())) {
    Character c;
    c.id = e.value("id", -1);
    c.name = e.value("name", "");
    c.kind = e.value("kind", "");
    if (e.contains("titantron") && e["titantron"].is_number_integer()) c.tron = e["titantron"].get<int>();
    if (c.tron >= 400 && c.tron < 500 && team_of_tron.count(c.tron)) c.note = "team tron: " + team_of_tron[c.tron];
    else if (c.tron >= 600 && c.tron < 900) c.note = "era tron";
    if (c.id >= 0) out.push_back(c);
  }
  std::sort(out.begin(), out.end(), [](const Character& a, const Character& b) { return Lower(a.name) < Lower(b.name); });
  return out;
}

inline std::vector<Arena> LoadArenas(const fs::path& ids_dir) {
  std::vector<Arena> out;
  std::ifstream f(ids_dir / "arenas.json");
  if (!f) return out;
  try {
    json j = json::parse(f);
    for (const auto& e : j.value("entries", json::array())) out.push_back({e.value("id", -1), e.value("name", "")});
  } catch (...) {}
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Tron files

inline constexpr const char* kTronExts[] = {".bik2", ".mp4", ".mov", ".mkv", ".alpha.mp4", ".alpha.mov", ".alpha.mkv"};

// "500.alpha.mp4" -> ".alpha.mp4", "343.mp4" -> ".mp4": the part of a tron file name after the number.
inline std::string TronExt(const fs::path& f) {
  const std::string n = Lower(PathStr(f.filename()));
  const size_t dot = n.find('.');
  return dot == std::string::npos ? std::string() : n.substr(dot);
}
inline bool IsStackedAlpha(const fs::path& f) { return TronExt(f).rfind(".alpha.", 0) == 0; }

struct VideoInfo {
  bool ok = false;
  int w = 0, h = 0;
  double fps = 0, seconds = 0;
  bool alpha = false;  // Bink: the file has an alpha plane
  std::string format, error;
};

// Bink header ('KB2x' = Bink 2, 'BIKx' = Bink 1): file size - 8, frame count, largest frame, frame count, width,
// height, fps num, fps den, flags (0x100000 = alpha: the title reveals).
inline VideoInfo BinkInfoFromHeader(const std::vector<uint8_t>& head) {
  VideoInfo v;
  uint32_t h[10] = {};
  if (head.size() < sizeof(h)) {
    v.error = "can't read";
    return v;
  }
  std::memcpy(h, head.data(), sizeof(h));
  const char* sig = reinterpret_cast<const char*>(h);
  const bool bink2 = sig[0] == 'K' && sig[1] == 'B' && sig[2] == '2';
  const bool bink1 = sig[0] == 'B' && sig[1] == 'I' && sig[2] == 'K';
  if (!bink1 && !bink2) {
    v.error = "not a Bink file";
    return v;
  }
  v.ok = true, v.format = std::string(bink2 ? "Bink 2 (" : "Bink 1 (") + sig[3] + ")";
  v.alpha = (h[9] & 0x100000u) != 0;
  if (v.alpha) v.format += ", alpha";
  v.w = int(h[5]), v.h = int(h[6]);
  v.fps = h[8] ? double(h[7]) / h[8] : 0;
  v.seconds = v.fps > 0 ? h[2] / v.fps : 0;
  return v;
}

inline VideoInfo BinkInfo(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::vector<uint8_t> head(40);
  f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  head.resize(size_t(std::max<std::streamsize>(0, f.gcount())));
  return BinkInfoFromHeader(head);
}

// Media Foundation: size, rate, length, and one frame as RGBA (for the thumbnail), at about `at_seconds`.
inline VideoInfo MfInfo(const fs::path& p, double at_seconds, std::vector<uint8_t>* rgba) {
  VideoInfo v;
  static bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
  if (!started) {
    v.error = "Media Foundation not available";
    return v;
  }
  IMFAttributes* attrs = nullptr;
  MFCreateAttributes(&attrs, 1);
  attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  IMFSourceReader* r = nullptr;
  HRESULT hr = MFCreateSourceReaderFromURL(p.wstring().c_str(), attrs, &r);
  attrs->Release();
  if (FAILED(hr)) {
    char b[64];
    std::snprintf(b, sizeof(b), "Windows can't open it (0x%08lX)", static_cast<unsigned long>(hr));
    v.error = b;
    return v;
  }
  r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  r->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
  IMFMediaType* native = nullptr;
  if (SUCCEEDED(r->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
    GUID sub{};
    native->GetGUID(MF_MT_SUBTYPE, &sub);
    v.format = sub == MFVideoFormat_H264 ? "H.264" : sub == MFVideoFormat_HEVC ? "HEVC" : sub == MFVideoFormat_AV1 ? "AV1"
               : sub == MFVideoFormat_VP90 ? "VP9" : "video";
    native->Release();
  }
  IMFMediaType* t = nullptr;
  MFCreateMediaType(&t);
  t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
  t->Release();
  IMFMediaType* cur = nullptr;
  if (SUCCEEDED(hr) && SUCCEEDED(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
    UINT32 w = 0, h = 0, n = 0, d = 0;
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(cur, MF_MT_FRAME_RATE, &n, &d);
    v.w = int(w), v.h = int(h), v.fps = d ? double(n) / d : 0;
    cur->Release();
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(r->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
      v.seconds = double(var.uhVal.QuadPart) * 1e-7;
    PropVariantClear(&var);
    v.ok = v.w > 0 && v.h > 0;
    if (rgba && v.ok) {
      PROPVARIANT pos;
      PropVariantInit(&pos);
      pos.vt = VT_I8;
      pos.hVal.QuadPart = LONGLONG(std::min(at_seconds, v.seconds * 0.5) * 1e7);
      r->SetCurrentPosition(GUID_NULL, pos);
      for (int tries = 0; tries < 128; ++tries) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        if (FAILED(r->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
          break;
        if (!s) continue;
        if (ts + 400000 < pos.hVal.QuadPart && tries < 120) {  // the seek lands on a key frame before the target
          s->Release();
          continue;
        }
        IMFMediaBuffer* b = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
          BYTE* data = nullptr;
          DWORD len = 0;
          if (SUCCEEDED(b->Lock(&data, nullptr, &len)) && len >= DWORD(v.w * v.h * 4)) {
            rgba->resize(size_t(v.w) * v.h * 4);
            for (int y = 0; y < v.h; ++y) {  // BGRX -> RGBA
              const BYTE* src = data + size_t(y) * v.w * 4;
              uint8_t* dst = rgba->data() + size_t(y) * v.w * 4;
              for (int x = 0; x < v.w; ++x)
                dst[4 * x] = src[4 * x + 2], dst[4 * x + 1] = src[4 * x + 1], dst[4 * x + 2] = src[4 * x],
                dst[4 * x + 3] = 255;
            }
            b->Unlock();
          }
          b->Release();
        }
        s->Release();
        if (!rgba->empty()) break;
      }
    }
  } else if (v.error.empty()) {
    v.error = "no video stream Windows can decode";
  }
  r->Release();
  return v;
}

inline VideoInfo ProbeVideo(const fs::path& p, std::vector<uint8_t>* rgba = nullptr) {
  return Lower(PathStr(p.extension())) == ".bik2" ? BinkInfo(p) : MfInfo(p, 1.0, rgba);
}

// The tron sheet (Session 18): a 320x320 character tron is the big screen (rows 0-223) and four 24-row banner strips.
inline constexpr int kSheetBigRows = 224, kSheetStripRows = 24, kSheetStrips = 4;

// Banner strips for an MP4 tron (the game reads titantron\NNN.banner.txt / NNN.banner.mp4; src\tron_shim.h).
enum class BannerMode { kOriginal, kVideo, kNone, kFile };
struct BannerChoice {
  BannerMode mode = BannerMode::kOriginal;
  fs::path file;  // kFile
};

inline BannerChoice GetBanner(const Paths& p, int tron) {
  BannerChoice b;
  std::error_code ec;
  const fs::path dir = p.mods / "titantron";
  const std::string stem = std::to_string(tron) + ".banner";
  std::ifstream f(dir / (stem + ".txt"));
  if (f) {
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    const std::string l = Lower(t);
    if (l == "video") b.mode = BannerMode::kVideo;
    else if (l == "none") b.mode = BannerMode::kNone;
    else if (!l.empty() && l != "original") b.mode = BannerMode::kFile, b.file = dir / fs::u8path(t);
    return b;
  }
  for (const char* e : {".mp4", ".mov", ".mkv"})
    if (fs::is_regular_file(dir / (stem + e), ec)) return {BannerMode::kFile, dir / (stem + e)};
  return b;
}

// Writes the choice; a separate video is copied to NNN.banner.<ext>. "original" removes every banner file.
inline std::string SetBanner(const Paths& p, int tron, BannerMode mode, const fs::path& src = {}) {
  std::error_code ec;
  const fs::path dir = p.mods / "titantron";
  fs::create_directories(dir, ec);
  const std::string stem = std::to_string(tron) + ".banner";
  if (mode == BannerMode::kFile) {
    const std::string ext = Lower(PathStr(src.extension()));
    if (ext != ".mp4" && ext != ".mov" && ext != ".mkv") return "use a .mp4, .mov or .mkv file";
    if (!fs::is_regular_file(src, ec)) return "file not found: " + PathStr(src);
    const fs::path dst = dir / (stem + ext);
    if (fs::weakly_canonical(src, ec) != fs::weakly_canonical(dst, ec)) {
      fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
      if (ec) return "copy failed: " + ec.message();
    }
    for (const char* e : {".mp4", ".mov", ".mkv"})
      if (ext != e) fs::remove(dir / (stem + e), ec);
    std::ofstream(dir / (stem + ".txt")) << PathStr(dst.filename()) << "\n";
    return "";
  }
  for (const char* e : {".mp4", ".mov", ".mkv"}) fs::remove(dir / (stem + e), ec);
  if (mode == BannerMode::kOriginal) {
    fs::remove(dir / (stem + ".txt"), ec);
    return "";
  }
  std::ofstream(dir / (stem + ".txt")) << (mode == BannerMode::kVideo ? "video" : "none") << "\n";
  return "";
}

inline void RemoveBanner(const Paths& p, int tron) {
  std::error_code ec;
  const fs::path dir = p.mods / "titantron";
  const std::string stem = std::to_string(tron) + ".banner";
  for (const char* e : {".txt", ".mp4", ".mov", ".mkv"}) fs::remove(dir / (stem + e), ec);
}

// Session 26: the game files may be a folder or a disc image; read them through GameFiles (disc_image.h).
inline GameFiles& GameFilesAt(const fs::path& root) {
  static GameFiles g;
  g.SetRoot(root);
  return g;
}

inline std::string OriginalTronRel(int tron) { return "movies\\titantron\\" + std::to_string(tron) + ".bik2"; }

inline bool OriginalTronExists(const Paths& p, int tron) { return GameFilesAt(p.game_data).Exists(OriginalTronRel(tron)); }

inline VideoInfo OriginalTronInfo(const Paths& p, int tron) {
  return BinkInfoFromHeader(GameFilesAt(p.game_data).ReadHead(OriginalTronRel(tron), 40));
}

// The replacement in the mods folder for tron `tron` (the same order the game checks), or empty.
inline fs::path OverrideFor(const Paths& p, int tron) {
  std::error_code ec;
  for (const char* ext : kTronExts) {
    auto f = p.mods / "titantron" / (std::to_string(tron) + ext);
    if (fs::is_regular_file(f, ec)) return f;
  }
  return {};
}

inline std::map<int, fs::path> AllOverrides(const Paths& p) {
  std::map<int, fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(p.mods / "titantron", ec)) {
    if (!e.is_regular_file()) continue;
    const std::string name = PathStr(e.path().filename());
    const std::string stem = name.substr(0, name.find('.')), ext = TronExt(e.path());
    if (stem.empty() || !std::all_of(stem.begin(), stem.end(), ::isdigit)) continue;
    if (std::find(std::begin(kTronExts), std::end(kTronExts), ext) == std::end(kTronExts)) continue;
    const int id = std::stoi(stem);
    if (!out.count(id) || OverrideFor(p, id) == e.path()) out[id] = e.path();
  }
  return out;
}

// Copies `src` to <mods>\titantron\NNN.<ext> and removes any other replacement for the same tron. Returns "" on
// success, else the reason. The game's own files are never touched.
// `stacked_alpha`: store it as NNN.alpha.<ext> (colour on top, alpha below); also when the file's own name has
// ".alpha.".
inline std::string AssignTron(const Paths& p, int tron, const fs::path& src, bool stacked_alpha = false) {
  std::error_code ec;
  if (tron < 0) return "no tron number";
  if (!fs::is_regular_file(src, ec)) return "file not found: " + PathStr(src);
  std::string ext = Lower(PathStr(src.extension()));
  if (ext != ".bik2" && (stacked_alpha || IsStackedAlpha(src))) ext = ".alpha" + ext;
  if (std::find(std::begin(kTronExts), std::end(kTronExts), ext) == std::end(kTronExts))
    return "use a .mp4, .mov, .mkv or .bik2 file";
  const fs::path dir = p.mods / "titantron";
  if (fs::weakly_canonical(src.parent_path(), ec) == fs::weakly_canonical(p.game_data / "movies" / "titantron", ec) &&
      PathStr(src.stem()) == std::to_string(tron) && ext == ".bik2")
    return "that is the original file already";
  fs::create_directories(dir, ec);
  const fs::path dst = dir / (std::to_string(tron) + ext);
  const fs::path tmp = dir / (std::to_string(tron) + ext + ".part");
  fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) return "copy failed: " + ec.message();
  for (const char* e : kTronExts) {
    if (ext == e) continue;
    fs::remove(dir / (std::to_string(tron) + e), ec);
  }
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return "rename failed: " + ec.message();
  }
  return "";
}

// Session 33: custom entrance music, <mods>\music\ID.ext (ID = the wrestler's character id; src\entrance_music.h).
inline constexpr const char* kMusicExts[] = {".mp3", ".m4a", ".wav", ".flac", ".wma", ".aac"};
inline fs::path MusicFor(const Paths& p, int id) {
  std::error_code ec;
  for (const char* ext : kMusicExts) {
    auto f = p.mods / "music" / (std::to_string(id) + ext);
    if (fs::is_regular_file(f, ec)) return f;
  }
  return {};
}
inline bool HasAnyMusic(const Paths& p) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(p.mods / "music", ec))
    for (const char* ext : kMusicExts)
      if (e.is_regular_file(ec) && Lower(PathStr(e.path().extension())) == ext) return true;
  return false;
}
inline std::string AssignMusic(const Paths& p, int id, const fs::path& src) {
  std::error_code ec;
  if (id < 0) return "no wrestler";
  if (!fs::is_regular_file(src, ec)) return "file not found: " + PathStr(src);
  const std::string ext = Lower(PathStr(src.extension()));
  if (std::find(std::begin(kMusicExts), std::end(kMusicExts), ext) == std::end(kMusicExts))
    return "use a .mp3, .m4a, .wav, .flac, .wma or .aac file";
  const fs::path dir = p.mods / "music";
  fs::create_directories(dir, ec);
  const fs::path dst = dir / (std::to_string(id) + ext), tmp = dir / (std::to_string(id) + ext + ".part");
  fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) return "copy failed: " + ec.message();
  for (const char* e : kMusicExts)
    if (ext != e) fs::remove(dir / (std::to_string(id) + e), ec);
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return "rename failed: " + ec.message();
  }
  return "";
}
inline void RevertMusic(const Paths& p, int id) {
  std::error_code ec;
  for (const char* e : kMusicExts) fs::remove(p.mods / "music" / (std::to_string(id) + e), ec);
}

inline int RevertTron(const Paths& p, int tron) {
  int n = 0;
  std::error_code ec;
  for (const char* e : kTronExts) n += fs::remove(p.mods / "titantron" / (std::to_string(tron) + e), ec) ? 1 : 0;
  RemoveBanner(p, tron);  // the banner choice belongs to the replacement
  return n;
}

inline int RevertAll(const Paths& p) {
  int n = 0;
  for (const auto& [id, f] : AllOverrides(p)) n += RevertTron(p, id);
  return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Scenarios, recordings, scripts

struct Scenario {
  std::string name, description;
  int arena = -1;
  std::vector<int> ids;
};

inline std::vector<Scenario> LoadScenarios(const Paths& p) {
  std::vector<Scenario> out;
  std::error_code ec;
  static const std::regex kArena(R"(^arena\s*=\s*(-?\d+))"), kId(R"(^id\s*=\s*(\d+))"),
      kDesc(R"(^description\s*=\s*\"(.*)\")");
  for (const auto& e : fs::directory_iterator(p.scenarios, ec)) {
    if (Lower(PathStr(e.path().extension())) != ".toml") continue;
    Scenario s;
    s.name = PathStr(e.path().stem());
    std::ifstream f(e.path());
    std::string line;
    bool summary = false;
    while (std::getline(f, line)) {
      std::smatch m;
      if (line.rfind("[", 0) == 0) summary = line.rfind("[summary", 0) == 0 || line.rfind("[[summary", 0) == 0;
      if (std::regex_search(line, m, kDesc)) s.description = m[1];
      if (!summary) continue;
      if (std::regex_search(line, m, kArena)) s.arena = std::stoi(m[1]);
      if (std::regex_search(line, m, kId)) s.ids.push_back(std::stoi(m[1]));
    }
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const Scenario& a, const Scenario& b) { return a.name < b.name; });
  return out;
}

inline std::vector<std::string> ListFiles(const fs::path& dir, const char* ext, bool strip_ext) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file() || Lower(PathStr(e.path().extension())) != ext) continue;
    out.push_back(PathStr(strip_ext ? e.path().stem() : e.path().filename()));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Seed sweep results (out\seeds\SCENARIO.csv, tools\seeds\sweep.py).
struct SeedRow {
  std::map<std::string, std::string> f;
  std::string get(const std::string& k) const {
    auto it = f.find(k);
    return it == f.end() ? "" : it->second;
  }
};

inline std::vector<SeedRow> LoadSeeds(const Paths& p, const std::string& scenario) {
  std::vector<SeedRow> rows;
  std::ifstream f(p.seeds / (scenario + ".csv"));
  std::string line;
  std::vector<std::string> head;
  auto split = [](const std::string& l) {
    std::vector<std::string> v;
    std::string cur;
    bool q = false;
    for (char c : l) {
      if (c == '"') q = !q;
      else if (c == ',' && !q) v.push_back(cur), cur.clear();
      else if (c != '\r') cur += c;
    }
    v.push_back(cur);
    return v;
  };
  if (std::getline(f, line)) head = split(line);
  while (std::getline(f, line)) {
    auto v = split(line);
    SeedRow r;
    for (size_t i = 0; i < head.size() && i < v.size(); ++i) r.f[head[i]] = v[i];
    rows.push_back(r);
  }
  return rows;
}

// find.py's filters: winner=SLOT loser=SLOT finish=N|pinfall min_clock=S max_clock=S min_near_falls=N (the same
// rules as tools\seeds\find.py, so the launcher and the script agree).
inline bool SeedMatches(const SeedRow& r, const std::string& filter) {
  if (r.get("status").rfind("ok", 0) != 0) return false;
  std::istringstream in(filter);
  std::string kv;
  while (in >> kv) {
    auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "finish" && v == "pinfall") v = "1";
    auto num = [&](const std::string& field) { return std::atof(r.get(field).c_str()); };
    if (k == "winner" || k == "loser" || k == "finish") {
      if (r.get(k) != v) return false;
    } else if (k == "min_clock") {
      if (num("clock_s") < std::atof(v.c_str())) return false;
    } else if (k == "max_clock") {
      if (num("clock_s") > std::atof(v.c_str())) return false;
    } else if (k == "min_near_falls") {
      if (num("near_falls") < std::atof(v.c_str())) return false;
    } else if (k == "min_pins") {
      if (num("pins") < std::atof(v.c_str())) return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The command line

enum Preset { kPlay = 0, kReplay, kReshoot, kSeed, kDirector, kPresetCount };
inline const char* kPresetNames[] = {"Play", "Replay", "Re-shoot", "Seed", "Director"};

struct Command {
  std::wstring exe, args, workdir;  // what CreateProcess runs
  std::string display;              // the exact line, shown and copyable
  bool console = false;             // a script: give it a console window
  std::string error;                // why it can't run
};

inline std::string Quote(const std::string& s) {
  if (s.find_first_of(" \t\"'") == std::string::npos && !s.empty()) return s;
  // Session 32 review: backslashes right before the closing quote must be doubled, or `"D:\My Games\dlc\"` reads the
  // last \" as a literal quote and swallows the rest of the command line (Windows argument rules).
  size_t trailing = 0;
  while (trailing < s.size() && s[s.size() - 1 - trailing] == '\\') ++trailing;
  return "\"" + s + std::string(trailing, '\\') + "\"";
}

// The game's flags for the Play tab's settings (everything else is left at the game's defaults).
// Session 31: the game's languages = the runtime's user_language ids that change its text (runs 2409-2419: 1, 3, 4,
// 5, 6; 2, 7, 9, 11, 12 and 16 show English). Automatic = Windows' display language when the game has it.
inline int AutoLanguage() {
  switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_GERMAN: return 3;
    case LANG_FRENCH: return 4;
    case LANG_SPANISH: return 5;
    case LANG_ITALIAN: return 6;
    default: return 1;
  }
}

// Session 32: the player's DLC packs (the switched-off ones by pack id). Session 57 (tester bug 2): the helper runs
// (logo import, Build previews) get them too: they load the same save, and the game refuses a save made with packs it
// does not see ("missing or damaged downloadable content").
inline std::vector<std::string> DlcFlags(const Settings& s, const Paths& p) {
  std::vector<std::string> f;
  if (!s.dlc) return f;
  f.push_back("--wwe2k14_dlc=true");
  f.push_back("--wwe2k14_dlc_dir=" + Quote(PathStr(p.dlc)));
  std::string off;  // rebuilt from the ids: a hand-edited settings file cannot add other flags (review)
  for (int id : wwe2k14::dlc::ParseIdList(s.dlc_off)) off += (off.empty() ? "" : ",") + std::to_string(id);
  if (!off.empty()) f.push_back("--wwe2k14_dlc_off=" + off);
  return f;
}

inline std::string GameFlags(const Settings& s, const Paths& p) {
  std::vector<std::string> f;
  // Session 25: where the game files and the save are (only when not the defaults), and a release's scenario folder.
  if (!s.game_data_dir.empty()) f.push_back("--game_data_root=" + Quote(s.game_data_dir));
  if (!s.user_data_dir.empty()) f.push_back("--user_data_root=" + Quote(s.user_data_dir));
  if (!p.repo_found) f.push_back("--wwe2k14_scenario_dir=" + Quote(PathStr(p.scenarios)));
  if (s.scenario == "@quick") {
    // Session 27 item 8: a match built from our numbers (src\scenario.h, --wwe2k14_launch=@quick).
    f.push_back("--wwe2k14_launch=@quick");
    f.push_back("--wwe2k14_quick_type=" + std::to_string(s.quick_type));
    // Review fix: without :PAD every slot is CPU; the player gets wrestler 1 on controller 1 unless "CPU vs CPU".
    std::string ids = s.quick_ids;
    if (!s.quick_cpu_only && !ids.empty()) {
      const size_t comma = ids.find(',');
      if (ids.substr(0, comma).find(':') == std::string::npos)
        ids.insert(comma == std::string::npos ? ids.size() : comma, ":0");
    }
    f.push_back("--wwe2k14_launch_participants=" + ids);
    if (s.quick_arena >= 0) f.push_back("--wwe2k14_launch_arena=" + std::to_string(s.quick_arena));
    if (s.skip_entrances) f.push_back("--wwe2k14_launch_skip_entrances=true");
  } else if (s.wm30 >= 0) {
    // Session 34: the game drives its menus to the 30 Years match (src\wm30_jump.h); no scenario at the same time.
    f.push_back("--wwe2k14_wm30=" + std::to_string(s.wm30));
  } else if (!s.scenario.empty()) {
    f.push_back("--wwe2k14_launch=" + s.scenario);
    if (s.arena_override) f.push_back("--wwe2k14_launch_arena=" + std::to_string(s.arena));
    if (!s.participants.empty()) f.push_back("--wwe2k14_launch_participants=" + s.participants);
    if (s.skip_entrances) f.push_back("--wwe2k14_launch_skip_entrances=true");
  }
  if (s.mods) {
    f.push_back("--wwe2k14_mods=true");
    if (!s.mods_dir.empty() || !p.repo_found) f.push_back("--wwe2k14_mods_dir=" + Quote(PathStr(p.mods)));
    if (HasAnyMusic(p)) f.push_back("--wwe2k14_entrance_music=true");  // Session 33: only once a track is assigned
  }
  for (auto& x : DlcFlags(s, p)) f.push_back(std::move(x));
  if (s.cinecam) f.push_back("--wwe2k14_cinecam=true");
  if (s.photo_mode) f.push_back("--wwe2k14_photo_mode=true");  // Session 33
  // Session 33: Discord status (the project's application id, or one the player set in Settings -> Game).
  if (s.discord) {
    const std::string id = s.discord_app_id.empty() ? std::string(kDiscordAppId) : s.discord_app_id;
    f.push_back("--wwe2k14_discord=true");
    if (!id.empty()) f.push_back("--wwe2k14_discord_app_id=" + id);
  }
  if (s.runin) f.push_back("--wwe2k14_runin=true");
  if (s.keyboard) f.push_back("--mnk_mode=true");
  // Session 31: language, graphics card, window.
  f.push_back("--user_language=" + std::to_string(s.language > 0 ? s.language : AutoLanguage()));
  if (!s.gpu.empty()) f.push_back("--wwe2k14_gpu=" + Quote(s.gpu));
  if (s.windowed) f.push_back("--fullscreen=false");
  if (s.aspect == 1) f.push_back("--wwe2k14_aspect=auto");  // Session 33: ultrawide (Hor+)
  // Session 28: Settings -> Resolution / Frame rate / Upscaling go through the mapping the game shares with its
  // in-game GRAPHICS menu (src\gfx_settings.h), so the two cannot drift apart. DLSS picks its own render and output
  // size (its mode), so the resolution choice only applies without it; wrestler motion vectors are always on with
  // DLSS / DLAA (Jarrod, Sessions 25 and 28: not a player choice).
  {
    wwe2k14::gfxs::Choice c;
    c.upscale = s.upscale, c.resolution = s.resolution, c.frame_rate = s.frame_rate, c.refresh_hz = s.refresh_hz;
    c.draw_height = s.draw_height;  // Session 54
    c.original_look = s.original_look;
    c.texture_filter = s.texture_filter;  // Session 33
    c.renderer = s.renderer;              // Session 37 (Session 61: NATIVE, or the performance test's own run)
    // Session 61: the advanced flag --wwe2k14_renderer=light|accurate in Extra flags picks the renderer's flags.
    if (const int r = wwe2k14::gfxs::RendererFromFlags(s.extra); r >= 0) c.renderer = r;
    // The player's own NVIDIA folder, else the files bundled with a release (Session 25, Jarrod: ship them). Session
    // 31: a folder of their own that has no NVIDIA files (moved, deleted) falls back to the bundled ones too.
    const std::string dir = HasStreamline(s.dlss_dir) || p.nvidia.empty() ? s.dlss_dir : PathStr(p.nvidia);
    for (const auto& [name, value] : wwe2k14::gfxs::GraphicsFlags(c, dir, g_monitor_hz))
      f.push_back("--" + name + "=" + (name == "wwe2k14_dlaa_dir" ? Quote(value) : value));
    // The flags above are the whole picture (a choice that is the game's default has none): the game must not apply
    // the settings file on top of them (review fix: the file can be stale, or older than the launcher's own choice).
    f.push_back("--wwe2k14_use_launcher_settings=false");
  }
  if (s.perf_report == 1 || (s.perf_report < 0 && g_perf_report_default)) f.push_back("--wwe2k14_perf_report=true");
  switch (s.preset) {
    case kReplay:
      if (s.replay_record) f.push_back("--wwe2k14_replay_record=" + s.replay_name);
      else f.push_back("--wwe2k14_replay_play=" + s.replay_play), f.push_back("--wwe2k14_replay_dlc_box=true");
      break;
    case kSeed:
      f.push_back("--wwe2k14_replay_mode=true");
      f.push_back("--wwe2k14_fixed_seed=" + std::to_string(s.seed));
      break;
    case kDirector:
      f.push_back("--wwe2k14_director=" + s.director);
      if (s.director_seed) f.push_back("--wwe2k14_fixed_seed=" + std::to_string(s.seed));
      break;
    default:
      break;
  }
  if (!s.extra.empty()) f.push_back(s.extra);
  std::string out;
  for (const auto& x : f) out += (out.empty() ? "" : " ") + x;
  return out;
}

inline Command BuildCommand(const Settings& s, const Paths& p, bool sweep = false) {
  Command c;
  c.workdir = p.launcher_dir.wstring();
  const std::string flags = GameFlags(s, p);
  auto pwsh = [&](const fs::path& script, const std::string& rest) {
    c.exe = L"pwsh.exe";
    std::string a = "-NoProfile -File " + Quote(PathStr(script)) + " " + rest;
    c.args = Widen(a);
    c.display = "pwsh " + a;
    c.console = true;
    c.workdir = p.repo.wstring();
  };
  if (sweep) {  // Seed preset: run the sweeper over a range
    if (s.scenario.empty()) c.error = "pick a scenario to sweep";
    std::string a = Quote(PathStr(p.repo / "tools" / "seeds" / "sweep.py")) + " " + s.scenario + " --seeds " +
                    std::to_string(s.sweep_from) + ".." + std::to_string(s.sweep_to);
    if (s.preset == kDirector && !s.director.empty()) a += " --director " + s.director;
    c.exe = L"py.exe", c.args = Widen(a), c.display = "py " + a, c.console = true, c.workdir = p.repo.wstring();
    return c;
  }
  if (s.preset == kReshoot) {
    if (s.replay_play.empty()) c.error = "pick a recording to re-shoot";
    if (s.reshoot_shots.empty()) c.error = "add at least one shot (CineCam path file)";
    std::string more;
    if (s.mods) more += "--wwe2k14_mods=true";
    pwsh(p.repo / "tools" / "replay" / "reshoot.ps1",
         "-Name " + s.replay_play + " -Shots " + s.reshoot_shots + " -Anchor " + s.reshoot_anchor +
             (more.empty() ? "" : " -More " + Quote(more)));
    return c;
  }
  if (s.preset == kReplay && !s.replay_record && s.replay_play.empty()) c.error = "pick a recording to play";
  if (s.preset == kDirector && s.director.empty()) c.error = "pick a director script";
  if (s.harness) {
    fs::path route = fs::path(Widen(s.harness_route));
    if (route.is_relative()) route = p.routes / route;
    pwsh(p.repo / "tools" / "bench" / "run_list.ps1",
         "-Scripts " + Quote(PathStr(route)) + (flags.empty() ? "" : " -Extra " + Quote(flags)));
    return c;
  }
  c.exe = p.game_exe.wstring();
  c.args = Widen(flags);
  c.display = Quote(PathStr(p.game_exe)) + (flags.empty() ? "" : " " + flags);
  std::error_code ec;
  if (!fs::exists(p.game_exe, ec)) c.error = "game not found: " + PathStr(p.game_exe);
  return c;
}

inline std::string RunCommand(const Command& c) {
  if (!c.error.empty()) return c.error;
  std::wstring line = L"\"" + c.exe + L"\" " + c.args;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  DWORD flags = c.console ? CREATE_NEW_CONSOLE : 0;
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, flags, nullptr,
                      c.workdir.empty() ? nullptr : c.workdir.c_str(), &si, &pi)) {
    return "could not start (error " + std::to_string(GetLastError()) + ")";
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return "";
}

}  // namespace launcher
