// wwe2k14 - the graphics settings the launcher and the in-game GRAPHICS menu share (Session 28).
//
// One source of truth: the launcher's own settings file, %APPDATA%\wwe2k14-launcher\settings.json (written by the
// launcher with nlohmann::json, so every value is `"key": value`). The launcher keeps writing and reading all of it;
// this header only touches the keys below, by text, so it works in the game exe (which has no JSON library) and leaves
// everything else in the file as it was.
//
//   upscale      0 off, 1 DLAA, 2 DLSS performance, 3 DLSS quality, 4 DLSS ultra performance,
//                5 FSR native, 6 FSR quality, 7 FSR performance (Session 33: AMD FSR 3, any graphics card)
//   resolution   0 720p, 1 1440p (the default), 2 4K
//   draw_height  Session 54 (Jarrod: "360p, 480, 540p, 720p, 1080p, 1440p, and 4k"): 0, or 360 / 480 / 540 (with
//                resolution 0) or 1080 (with resolution 1): the NATIVE renderer draws at that height and makes its
//                copies at the resolution's whole size (SDK gpu_native_truth_draw_height). A key of its own, so a
//                build from before it reads 720p / 1440p (unknown resolution values would clamp to 4K there)
//   frame_rate   0 classic (30 fps entrances, 60 in matches), 1 60 everywhere (default), 2 high refresh,
//                3 30 everywhere (Session 37: weak PCs; the launcher only)
//   refresh_hz   high refresh: game steps per second (0 = the monitor's)
//   original_look  the original 720p / 4x MSAA / 30 fps entrance look (Advanced)
//   texture_filter 0 standard (the forced 4x anisotropic filtering every run had), 1 8x, 2 16x (Session 33)
//   renderer     0 accurate, 1 light (Session 37), 2 native (Session 41: accurate + the native renderer). Session 51
//                (Jarrod, 2026-10-22: "NATIVE for everyone"): 2 is the default; ACCURATE and LIGHT are fallbacks
//   settings_version  Session 51: 2 = the file has had the one-time move of the old default renderer (0) to NATIVE
//   dlss_dir     the player's own NVIDIA folder (empty = the files next to the game)
//
// GraphicsFlags() is the mapping from those values to the game's own settings; the launcher puts the result on the
// command line and the game applies it to itself when it is started without the launcher (wwe2k14_app.h).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <shlobj.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace wwe2k14::gfxs {

namespace fs = std::filesystem;

struct Choice {
  int upscale = 0;
  int resolution = 1;
  int frame_rate = 1;
  int refresh_hz = 0;
  bool original_look = false;
  int texture_filter = 0;  // Session 33
  int draw_height = 0;     // Session 54: 0, 360 / 480 / 540 (resolution 0), 1080 (resolution 1); NATIVE only
  int renderer = 2;        // Session 37: 0 accurate, 1 light (--wwe2k14_light_renderer); Session 41: 2 native
                           // (the accurate renderer with the native renderer drawing the scene, --gpu_native=2);
                           // Session 51: 2 by default (Jarrod: "NATIVE for everyone")
  std::string dlss_dir;
  bool operator==(const Choice& o) const {
    return upscale == o.upscale && resolution == o.resolution && draw_height == o.draw_height &&
           frame_rate == o.frame_rate && texture_filter == o.texture_filter;
  }
};

// The launcher's settings file (rex: Roaming AppData).
inline fs::path DefaultSettingsPath() {
  PWSTR appdata = nullptr;
  fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) dir = appdata;
  CoTaskMemFree(appdata);
  if (dir.empty()) dir = fs::temp_directory_path();
  return dir / "wwe2k14-launcher" / "settings.json";
}

inline std::string ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// "key": <integer>  ->  true and the value; false when the key is absent.
inline bool GetInt(const std::string& json, const char* key, int& out) {
  const std::regex re(std::string("\"") + key + "\"\\s*:\\s*(-?\\d+)");
  std::smatch m;
  if (!std::regex_search(json, m, re)) return false;
  out = std::atoi(m[1].str().c_str());
  return true;
}
inline bool GetBool(const std::string& json, const char* key, bool& out) {
  const std::regex re(std::string("\"") + key + "\"\\s*:\\s*(true|false)");
  std::smatch m;
  if (!std::regex_search(json, m, re)) return false;
  out = m[1].str() == "true";
  return true;
}
inline bool GetString(const std::string& json, const char* key, std::string& out) {
  const std::regex re(std::string("\"") + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
  std::smatch m;
  if (!std::regex_search(json, m, re)) return false;
  std::string raw = m[1].str(), s;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      ++i;
      s += raw[i] == 'n' ? '\n' : raw[i] == 't' ? '\t' : raw[i];
    } else {
      s += raw[i];
    }
  }
  out = s;
  return true;
}

// Session 51 (Jarrod, 2026-10-22: "NATIVE for everyone"): the settings version whose files already had the one-time
// move of the old default renderer to NATIVE. A file below it with renderer 0 (ACCURATE, the default until Session 50)
// reads as NATIVE; LIGHT and NATIVE stay as they are. The launcher writes the version back with its next save, so an
// ACCURATE picked by hand after that stays (one picked by hand before it cannot be told from the old default).
// Session 54: version 3 = the file has had the launcher's one-time device moves (launcher_main.cpp MigrateForDevice:
// LIGHT -> NATIVE on a card without interlock, 720p -> 540p on integrated graphics with NATIVE).
inline constexpr int kSettingsVersion = 3;
inline int MigrateRenderer(const std::string& json, int renderer) {
  int version = 0;
  GetInt(json, "settings_version", version);
  (void)version;  // (Session 51's move: files from before version 2 with renderer 0)
  (void)renderer;
  // Session 61 (Jarrod, 2026-10-25: "remove the renderer option entirely, with light and accurate as advanced flags
  // only"): the file's renderer is not a choice any more - always NATIVE; LIGHT / ACCURATE only through
  // --wwe2k14_renderer=light|accurate (RendererFromFlags).
  return 2;
}

// Session 61: the advanced flag --wwe2k14_renderer=native|light|accurate in a flag string (the launcher's Extra flags)
// -> 2 / 1 / 0; -1 when it is not there (or names no renderer).
inline int RendererFromFlags(const std::string& flags) {
  const std::string key = "--wwe2k14_renderer=";
  const size_t at = flags.rfind(key);
  if (at == std::string::npos || (at > 0 && flags[at - 1] != ' ')) return -1;
  const size_t start = at + key.size();
  const std::string value = flags.substr(start, flags.find(' ', start) == std::string::npos
                                                    ? std::string::npos
                                                    : flags.find(' ', start) - start);
  return value == "native" ? 2 : value == "light" ? 1 : value == "accurate" ? 0 : -1;
}

// Which of the keys the file has (a key that is absent says nothing: the caller keeps what the game is running).
struct Present {
  bool upscale = false, resolution = false, frame_rate = false, refresh_hz = false, original_look = false;
  bool texture_filter = false;
  bool draw_height = false;  // Session 54
};

// Session 54: the resolutions in menu order - the file's `resolution` (the whole size copies are made at) and
// `draw_height` (0: that size; else what NATIVE draws at). native_only: the other renderers draw only whole sizes.
struct ResolutionEntry {
  const char* name;
  int resolution, draw_height;
  bool native_only;
};
inline constexpr ResolutionEntry kResolutions[] = {{"360P", 0, 360, true},   {"480P", 0, 480, true},
                                                   {"540P", 0, 540, true},   {"720P", 0, 0, false},
                                                   {"1080P", 1, 1080, true}, {"1440P", 1, 0, false},
                                                   {"4K", 2, 0, false}};
inline constexpr int kResolutionCount = int(std::size(kResolutions));
// The draw height that goes with a resolution, or 0.
inline int ValidDrawHeight(int resolution, int draw_height) {
  for (const ResolutionEntry& e : kResolutions)
    if (e.draw_height && e.draw_height == draw_height && e.resolution == resolution) return draw_height;
  return 0;
}
// The menu entry of a (resolution, draw height) pair; one NATIVE alone can draw shows as its whole size otherwise.
inline int ResolutionIndex(int resolution, int draw_height, bool native = true) {
  if (!native) draw_height = 0;
  for (int i = 0; i < kResolutionCount; ++i)
    if (kResolutions[i].resolution == resolution && kResolutions[i].draw_height == draw_height) return i;
  for (int i = 0; i < kResolutionCount; ++i)
    if (kResolutions[i].resolution == resolution && !kResolutions[i].draw_height) return i;
  return 5;  // 1440p
}

inline Present Parse(const std::string& json, Choice& c) {
  Present p;
  p.upscale = GetInt(json, "upscale", c.upscale);
  p.resolution = GetInt(json, "resolution", c.resolution);
  p.frame_rate = GetInt(json, "frame_rate", c.frame_rate);
  p.refresh_hz = GetInt(json, "refresh_hz", c.refresh_hz);
  p.original_look = GetBool(json, "original_look", c.original_look);
  p.texture_filter = GetInt(json, "texture_filter", c.texture_filter);
  p.draw_height = GetInt(json, "draw_height", c.draw_height);  // Session 54
  GetInt(json, "renderer", c.renderer);  // Session 37 (the launcher's Settings -> Graphics; not in the game's menu)
  c.renderer = MigrateRenderer(json, c.renderer);  // Session 51
  GetString(json, "dlss_dir", c.dlss_dir);
  c.upscale = std::clamp(c.upscale, 0, 7);
  c.resolution = std::clamp(c.resolution, 0, 2);
  c.draw_height = ValidDrawHeight(c.resolution, c.draw_height);
  c.frame_rate = std::clamp(c.frame_rate, 0, 3);  // Session 37: 3 = 30 everywhere (weak PCs)
  c.texture_filter = std::clamp(c.texture_filter, 0, 2);
  c.renderer = std::clamp(c.renderer, 0, 2);  // Session 41: 2 native
  return p;
}

// A path as UTF-8 text. path::string() converts to the ANSI code page and throws for a name that page cannot hold (a
// Windows user name with other letters in it); u8string() cannot (Session 28 review).
inline std::string Utf8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// Sets a true / false key that is already in the JSON text (nothing is added when it is not there).
inline std::string SetBoolIfPresent(std::string json, const char* key, bool value) {
  const std::regex re(std::string("(\"") + key + "\"\\s*:\\s*)(true|false)");
  std::smatch m;
  if (!std::regex_search(json, m, re)) return json;
  return json.substr(0, size_t(m.position(2))) + (value ? "true" : "false") + json.substr(size_t(m.position(2) + m.length(2)));
}

// Sets (or adds) an integer key in the JSON text, leaving the rest as it was.
inline std::string SetInt(std::string json, const char* key, int value) {
  const std::regex re(std::string("(\"") + key + "\"\\s*:\\s*)(-?\\d+)");
  std::smatch m;
  if (std::regex_search(json, m, re))  // spliced by hand: "$1" + digits would read as group 13 and so on
    return json.substr(0, size_t(m.position(2))) + std::to_string(value) + json.substr(size_t(m.position(2) + m.length(2)));
  const size_t brace = json.find('{');
  const std::string entry = std::string("\"") + key + "\": " + std::to_string(value);
  if (brace == std::string::npos) return "{\n  " + entry + "\n}\n";
  const size_t next = json.find_first_not_of(" \t\r\n", brace + 1);
  const bool empty_object = next == std::string::npos || json[next] == '}';
  return json.substr(0, brace + 1) + "\n  " + entry + (empty_object ? "" : ",") + json.substr(brace + 1);
}

// Session 37: sets (or adds) a string key in the JSON text ("..." with \ and " escaped, control characters dropped).
inline std::string SetString(std::string json, const char* key, const std::string& value) {
  std::string esc;
  for (char ch : value) {
    if (ch == '"' || ch == '\\') esc += '\\', esc += ch;
    else if (static_cast<unsigned char>(ch) >= 0x20) esc += ch;
  }
  const std::regex re(std::string("(\"") + key + "\"\\s*:\\s*)\"(?:[^\"\\\\]|\\\\.)*\"");
  std::smatch m;
  if (std::regex_search(json, m, re))
    return json.substr(0, size_t(m.position(0) + m.length(1))) + "\"" + esc + "\"" +
           json.substr(size_t(m.position(0) + m.length(0)));
  const size_t brace = json.find('{');
  const std::string entry = std::string("\"") + key + "\": \"" + esc + "\"";
  if (brace == std::string::npos) return "{\n  " + entry + "\n}\n";
  const size_t next = json.find_first_not_of(" \t\r\n", brace + 1);
  const bool empty_object = next == std::string::npos || json[next] == '}';
  return json.substr(0, brace + 1) + "\n  " + entry + (empty_object ? "" : ",") + json.substr(brace + 1);
}

// Session 37: writes the JSON text back the way Save does (temp file + rename, the old file never deleted first).
inline bool WriteJson(const fs::path& path, const std::string& json) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path tmp = path;
  tmp += L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << json;
    if (!f) return false;
  }
  for (int attempt = 0; attempt < 5; ++attempt) {
    ec.clear();
    fs::rename(tmp, path, ec);
    if (!ec) return true;
    Sleep(50);
  }
  fs::remove(tmp, ec);
  return false;
}

// Writes the three choices into the file (created with the folder when it does not exist). Temp file + rename, so a
// launcher reading at the same moment sees the old or the new file, never half of one. The rename replaces the old
// file in one step (retried a few times when something such as a virus scanner has the new file open); the old file
// is never deleted first, so a failed save leaves it as it was. "original_look": true in the file would make the game
// ignore resolution and frame rate on every start, so choosing them here turns it off (`cleared` says so).
inline bool Save(const fs::path& path, const Choice& c, bool* cleared = nullptr) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::string json = ReadFile(path);
  bool was_original = false;
  if (cleared) *cleared = false;
  if (GetBool(json, "original_look", was_original) && was_original) {
    json = SetBoolIfPresent(json, "original_look", false);
    if (cleared) *cleared = true;
  }
  json = SetInt(json, "upscale", c.upscale);
  json = SetInt(json, "resolution", c.resolution);
  json = SetInt(json, "draw_height", ValidDrawHeight(c.resolution, c.draw_height));  // Session 54
  json = SetInt(json, "frame_rate", c.frame_rate);
  json = SetInt(json, "refresh_hz", c.refresh_hz);  // Session 31: 120 vs UNCAPPED (the monitor's rate, 0)
  json = SetInt(json, "texture_filter", c.texture_filter);  // Session 33
  fs::path tmp = path;
  tmp += L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << json;
    if (!f) return false;
  }
  for (int attempt = 0; attempt < 5; ++attempt) {
    ec.clear();
    fs::rename(tmp, path, ec);
    if (!ec) return true;
    Sleep(50);
  }
  fs::remove(tmp, ec);
  return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Machine facts.

// An NVIDIA RTX card (DLSS / DLAA), by DXGI (the same test the launcher uses).
inline bool HasRtxCard() {
  bool rtx = false;
#if defined(_WIN32)  // Session 63 (Android port): DXGI is Windows only; no RTX card on a phone
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  if (HMODULE dxgi = LoadLibraryW(L"dxgi.dll")) {
    auto create = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    struct Factory;  // the interface is used through its COM vtable below, without the DXGI headers
    void* factory = nullptr;
    // IID_IDXGIFactory1 = 770aae78-f26f-4dba-a829-253c83d1b387
    static const GUID iid = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    if (create && SUCCEEDED(create(iid, &factory)) && factory) {
      // IDXGIFactory1::EnumAdapters1 is vtable slot 12; IDXGIAdapter1::GetDesc1 slot 10; Release slot 2.
      auto vt = [](void* o) { return *reinterpret_cast<void***>(o); };
      for (UINT i = 0;; ++i) {
        void* adapter = nullptr;
        using EnumFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, void**);
        if (FAILED(reinterpret_cast<EnumFn>(vt(factory)[12])(factory, i, &adapter)) || !adapter) break;
        struct Desc1 {
          WCHAR Description[128];
          UINT VendorId, DeviceId, SubSysId, Revision;
          SIZE_T Dedicated, DedicatedSys, Shared;
          LUID Luid;
          UINT Flags;
        } d{};
        using GetDescFn = HRESULT(STDMETHODCALLTYPE*)(void*, Desc1*);
        if (SUCCEEDED(reinterpret_cast<GetDescFn>(vt(adapter)[10])(adapter, &d)) && !(d.Flags & 2u)) {
          char name[128];
          WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
          if (d.VendorId == 0x10DE && std::string(name).find("RTX") != std::string::npos) rtx = true;
        }
        using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);
        reinterpret_cast<ReleaseFn>(vt(adapter)[2])(adapter);
      }
      using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);
      reinterpret_cast<ReleaseFn>(vt(factory)[2])(factory);
    }
  }
#endif  // _WIN32
  return rtx;
}

// The main monitor's refresh rate.
inline int MonitorHz() {
#if defined(_WIN32)
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return int(dm.dmDisplayFrequency);
#endif  // Session 63 (Android port): 60 there until ANDROID_PLAN F5 picks the phone's rate
  return 60;
}

// Session 33: the upscale value is one of the FSR modes / one of the NVIDIA ones (DLAA or DLSS).
inline bool IsFsr(int upscale) { return upscale >= 5 && upscale <= 7; }
inline bool IsNvidia(int upscale) { return upscale >= 1 && upscale <= 4; }
inline fs::path ExeFolder();
// AMD's FSR runtime next to the game (shipped with a release; copied by the build).
inline bool HasFsrRuntime() {
  std::error_code ec;
  return fs::is_regular_file(ExeFolder() / "amd_fidelityfx_vk.dll", ec);
}

inline fs::path ExeFolder() {
  wchar_t buf[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(buf).parent_path();
}

// Where the NVIDIA Streamline runtime is: the player's own folder, else the files next to the game (a release:
// nvidia\ beside the exe; the dev layout: the exe's folder). Empty when there is none.
inline std::string StreamlineDir(const std::string& explicit_dir, const std::string& settings_dir) {
  std::error_code ec;
  auto has = [&](const fs::path& d) { return !d.empty() && fs::is_regular_file(d / "sl.interposer.dll", ec); };
  if (!explicit_dir.empty()) return explicit_dir;  // a flag the player or the launcher gave: used as is
  if (!settings_dir.empty() && has(fs::path(settings_dir))) return settings_dir;
  const fs::path here = ExeFolder();
  if (has(here / "nvidia")) return Utf8(here / "nvidia");
  if (has(here)) return Utf8(here);
  return {};
}

// ---------------------------------------------------------------------------------------------------------------
// The mapping to the game's settings (the same flags the launcher builds). Each pair is {setting name, value}.

// What the NATIVE renderer is (Session 51: one list for the launcher's NATIVE, the settings file and the game's
// --wwe2k14_preset_low, which keeps only its 720p / 30 fps part besides).
inline std::vector<std::pair<std::string, std::string>> NativeRendererFlags() {
  return {
      // Session 41 (low-spec step 2): the accurate renderer with the native renderer drawing the scene. The SDK turns it
      // off by itself where the accurate path cannot run (no interlock: the light renderer's path and fixes).
      {"gpu_native", "2"},
      // Low-spec step 3: exact cheap clears (resolve hashes identical, runs 2725-2732).
      {"gpu_fast_clear", "true"},
      // Session 47 (Jarrod, 2026-10-22: the playtest zip "with our fully native GPU renderer"): truth frames (no EDRAM;
      // the SDK refuses them itself with DLSS / DLAA and needs gpu_native 2, so the light renderer and cards without
      // interlock are unaffected).
      {"gpu_native_truth", "true"},
      // Session 50 (Jarrod, 2026-10-22: "We'll turn it on"): the D3D library's GPR-flush point draws not executed
      // (Session 48: pictures and hashes identical, command thread -4.3%).
      {"gpu_native_half2_skip_gpr_draws", "true"},
      // Session 50 step 1 (Jarrod, 2026-10-22: "Turn step 1 on"): the lean draw path and the geometry-page trust
      // (docs\native\LEAN_DRAWS.md: exclusive A/B 3410, command thread -14.3% match; the lean path acts only in truth
      // frames).
      {"gpu_native_half2", "2"},
      {"gpu_native_lean_draws", "true"},
      {"gpu_native_trust_geometry_pages", "true"},
      // Session 49's texture-only resolves (Jarrod, 2026-10-22 ~18:40: "Switch it on now"): resolves whose output only a
      // texture reads skip their write-back into guest memory (exclusive A/B 3377 at 720p: GPU -9.0% Brock, -7.8%
      // Bryan, -12.2% match; pictures and resolve hashes identical; 1x only - no effect at a resolution scale yet).
      {"gpu_native_truth_skip_writeback", "true"},
      // Session 52 step 1 (docs\native\PERF_S52.md: the NATIVE frame is drains between small steps, the 4090's SMs 94%
      // idle): barriers held back and joined until the next GPU work; the native renderer's own timestamps off; at a
      // resolution scale the RGBA8 resolves straight from the truth image (no staging buffer); packed texture fills
      // straight into the texture's storage view; 1x direct-load textures (storage view) for the resolve kinds even
      // before their page's first resolve, so most resolves become texture-only (13.8 -> 43.5 a match frame). Pictures
      // 6 / 6 identical (3544 / 3543 at 1440p, 3552 / 3546 at 720p), resolve hashes 467 / 467 vs 3131 (3554); the
      // direct 1x texture loads (Session 44, LOW's) now for NATIVE at every frame rate - the texture-only resolves need
      // them.
      {"vulkan_merge_adjacent_barriers", "true"},
      {"gpu_native_gpu_timers", "false"},
      {"gpu_native_truth_direct_resolve_scaled", "true"},
      {"gpu_native_truth_fill_direct", "true"},
      {"vulkan_texture_load_direct_unscaled", "true"},
      {"vulkan_texture_load_direct_unscaled_any_page", "true"},
      // Session 52 step 2: at a resolution scale the full k_16_16 / k_16_16_16_16 resolves straight from the truth
      // image too, and the RGBA8 ones also write the scaled texture they feed (no reload from the scaled buffer); at 1x
      // a texture-only resolve records no shared-memory use (no barriers for it). Pictures 6 / 6 identical (3557, 3575,
      // 3567), resolve hashes 467 / 467 (3568); exclusive A/B 3577 at 1440p -2.6 / -2.2 / -3.0%, 720p neutral (3576).
      {"gpu_native_truth_direct_resolve_scaled_full", "true"},
      {"gpu_native_truth_direct_resolve_scaled_texture", "true"},
      {"gpu_native_truth_defer_dest_use", "true"},
      // Session 52 step 2e: at a resolution scale the k_8 / k_4_4_4_4 resolves fill their textures (the mip chains are
      // no longer reloaded from the scaled buffer after each level: ~10 loads, 62 MB a 1440p match frame); the other
      // kinds stay off (no gain, 3590 / 3591). Pictures 6 / 6 identical (3589); A/B 3587 fast-forwarded -19 / -10 / -16%,
      // exclusive 3592 -1.5 / -4.9 / -5.5%.
      {"gpu_native_truth_texture_fill_scaled", "true"},
      {"gpu_native_truth_texture_fill_scaled_kinds", "32"},
      // Session 52 step 3 (1x): texture-only resolves recorded later in batches with their texture barriers, and a
      // clear-first pass on an image such a resolve reads takes a pooled image (the pixels outside the clear copied
      // from the original at the flush). Pictures 6 / 6 identical (3602, 3608), resolve hashes 467 / 467 (3603, 3607);
      // exclusive A/B 3609 at 720p -3.1 / -4.1 / -7.4%; reviewed (no route bug, 5 hardening fixes in SDK 4dead3e).
      {"gpu_native_truth_batch_resolves", "true"},
      {"gpu_native_truth_batch_rename", "true"},
      // Session 56 step 1 (the truth passes' render pass dependencies were a GPU wait each, and the match's scene pass
      // was split 7 times): truth passes without external dependencies, their synchronisation as image barriers only
      // where needed; a clear-first pass that renames its colour image renames its depth image too (the tiny
      // clear-first passes no longer share one); textures written by compute or copies made sampleable before a pass
      // opens. Pictures 6 / 6 byte-identical (3774 / 3775), resolve hashes 467 / 467 (3776 / 3777); in-run A/B 3778
      // (720p, fast-forwarded): Brock -13.3%, Bryan -5.9%, match -18.0%; reviewed (5 fixes, SDK acfe491).
      {"gpu_native_truth_lazy_pass_deps", "true"},
      {"gpu_native_truth_lazy_pass_style", "1"},
      {"gpu_native_truth_rename_depth", "true"},
      {"vulkan_texture_sample_written_early", "true"},
      // Session 56 step 3a: lean draws are not sent to the full IssueDraw for writes that change nothing they keep
      // (SHADER_CONSTANT_FLUSH_FETCH_*, shader loads, INVALIDATE_STATE): "state written" draws ~290 -> ~160 a match
      // frame; pictures 6 / 6 identical (3783), hashes 467 / 467 (3782); command thread match -3.3% (3781).
      {"gpu_native_lean_benign_writes", "true"},
      // Session 56 step 1f: texture-only resolves at a resolution scale too (the scaled resolve buffer written from the
      // texture only when something reads it), so batching and renames work at 1440p / 4K: pictures 1440p
      // byte-identical (3821), hashes 467 / 467 (3827); in-run A/B 1440p match -14.4% / -10.4%, Bryan -4.3% / -5.8%
      // (3822 / 3826); reviewed (64-bit offsets for 4K, the readback reader, SDK fixes).
      {"gpu_native_truth_skip_writeback_scaled", "true"},
      // Session 59: at a resolution scale a resolve's write use of the scaled resolve buffer is held until a dispatch
      // writes it (texture-only scaled resolves: none, so the truth passes around them need no barrier: 1440p 1 -> 21
      // free passes a match frame); texture fills into a storage view join the batch of texture-only resolves.
      // In-run A/Bs: 3963 (1440p) Brock -9.3%, Bryan -3.0%, match -8.2%; 3974 (720p) match -3.4%, command thread -2.3%.
      // Pictures 6 / 6 byte-identical at 720p and 1440p (3984 / 3986, 3987 / 3988), resolve hashes 467 / 467 (3989 /
      // 3990, 3991 / 3992); reviewed (fixes dfa7bd1, 063a235).
      {"gpu_native_truth_defer_dest_use_scaled", "true"},
      {"gpu_native_truth_batch_fills", "true"},
      // Session 59 step 1b: at a resolution scale the full k_16_16 / k_16_16_16_16 copies (the shadow atlas parts) also
      // write the scaled texture they feed - 1440p texture reloads 5 -> 0-1 a match frame (42 -> 0-14 MB). Pictures 6 / 6
      // byte-identical at 1440p (4009 vs 3987), resolve hashes 467 / 467 (4010 vs 3991); A/B 4011 (1440p): match -4.0%,
      // Bryan -3.2%, Brock -19.3%.
      {"gpu_native_truth_direct_resolve_scaled_full_texture", "true"},
      // Session 59 step 1d: at 1x the full k_8 / k_4_4_4_4 copies (shadow maps, a mip chain) straight from the RGBA8
      // truth image into memory - staged resolves 11.2 -> 1.0 a 720p match frame. Resolve hashes 467 / 467 (4019 vs
      // 3989), pictures 6 / 6 byte-identical (4020 vs 3984); A/B 4021 (720p): match -3.8%, Brock -10.1%, Bryan -0.5%.
      {"gpu_native_truth_direct_resolve_full_1x", "true"},
      // Session 59 medium items a / b (Jarrod: "Do the medium"): 1x k_8 textures get an R8_UINT storage view, and the
      // k_8 and depth texture fills write the texture through its storage view (no staging buffer, no buffer copy);
      // the 1x depth copy goes straight from the depth image into memory (no staging stores, no stock copy): staged
      // resolves 1.0 -> 0 and storage-view fills 12.6 -> 17.5 a 720p match frame (census 4041). Resolve hashes 467 / 467
      // (4051 vs 3989, 4052 vs 3991), pictures 6 / 6 byte-identical at 720p and 1440p (4053 vs 3984, 4054 vs 3987);
      // reviewed (fixes SDK e03ea01). In-run A/Bs (720p): 4045 match -11.4%, Brock -13.7%, Bryan +0.3%; 4055 match
      // -9.6%, Brock -10.9%, Bryan -12.6%.
      {"vulkan_texture_native_storage_small", "true"},
      {"gpu_native_truth_fill_direct_small", "true"},
      {"gpu_native_truth_fill_direct_depth", "true"},
      {"gpu_native_truth_direct_resolve_depth", "true"},
      // Session 59 medium item c: at a resolution scale the k_8 / k_4_4_4_4 copies straight into the scaled resolve
      // buffer (the stock scaled layout), and the depth copy above at a scale too: 1440p staged write-backs 12.6 -> 1.0
      // (entrances), 11.9 -> 0 (match) a frame (4060 / 4061). Exact as above (1440p 4052, 4054). Timed census 4062 /
      // 4063 (native GPU timers): write-backs 0.854 -> 0.708 ms a frame (entrances), match 0.459 -> 0.480 (noise: the
      // untouched fills moved 0.03); 1440p in-run A/Bs too noisy to read (4056 - 4059).
      {"gpu_native_truth_direct_resolve_scaled_small", "true"},
      // Session 56 (Jarrod, 2026-10-24 09:05: "let's have it on for native"): the per-draw constants in the GPU's
      // host-visible memory (the BAR window when there is no Resizable BAR) instead of system memory read across PCIe:
      // separate runs 3809-3812 GPU -7..-8% (match, Bryan), command thread +10..15% (the CPU writes them across PCIe).
      // Integrated GPUs share memory: no change there. Takes effect at start (a restart setting).
      {"vulkan_uniform_pool_device_local", "true"},
      {"vulkan_device_local_upload_min_heap_mb", "128"},
      // Session 54 (part B): on a card without fragment shader interlock (the host render target path) the truth
      // frames run too (no effect on cards with interlock). Forced fbo on the 4090: resolve hashes 467 / 467, parity
      // shots 6 / 6 byte-identical to NATIVE on interlock; vs LIGHT GPU -13.3% match, command thread -4.7%.
      {"gpu_native_truth_host_rt", "true"},
      // Session 60 step 2: the crowd at the engine level - of each crowd group (audience_draw_block) the game records the
      // first member only, the command processor adds the others as instances of that draw in one go (SDK
      // gpu_scene_crowd_batch, on by default). Pictures 6 / 6 pixel-identical (4091 vs 4092); fast-forwarded normal
      // play (4085 vs 4089): Brock's crowd opening shot 157 -> 240 fps (command thread -20%, draw packets 4,244 ->
      // 1,257), Bryan's entrance +4% (pan +5%, command thread -10%), match +2%.
      {"wwe2k14_scene_crowd", "1"},
      // Session 66 (NATIVE 2, G2.1): a truth pass whose first draw clears the whole surface does not load what it
      // clears (load op DONT_CARE: memory traffic on tile and bandwidth-poor GPUs; the 4090 is not bandwidth-bound).
      // check_item g2_dont_care: pictures 6 / 6 pixel-identical (4173 / 4174), resolve hashes 467 / 467 (4172 / 4175),
      // gameplay identical (4176 / 4177), no new validation-layer message (4183); in-run A/B (4178, 720p) GPU -0.6 /
      // -1.2 / -2.0% (Brock / Bryan / match, within noise on the 4090).
      {"gpu_native_truth_pass_dont_care", "true"},
      // Session 66 (B2.1): the vertex fetch's endian swap fixed in the shader from the draw's fetch constants (the
      // census: every draw 8-in-32, one key per shader, so no extra translations). check_item b2_endian: pictures 6 / 6
      // pixel-identical (4180), hashes 467 / 467 (4179), gameplay identical (4181); A/B (4182) GPU -8.2 / +1.3 / -3.3%.
      {"gpu_native_vertex_endian_spec", "true"},
      // Session 66 (H1.1): resolve readbacks (the app's load windows, src/create_readback.h) batched - one GPU wait for
      // many copies (~21-28 per wait, run 4193), flushed before any command-processor write the guest can observe.
      // check_item h1_batch (after fix 01170fd): pictures 6 / 6 (4221 / 4223), hashes 467 / 467 (4220 / 4222),
      // gameplay identical (4224 / 4225).
      {"readback_resolve_batch", "true"},
      // Session 66 (pixel shader specialisation): the texture fetch's sign / gamma handling fixed from the draw's fetch
      // constants (one key per shader; NATIVE pixel shaders 1,695 -> 1,066 AMD instructions, RGA gfx1103). check_item
      // ps_texspec: pictures 6 / 6, hashes 467 / 467, gameplay identical; A/B GPU -6.8% Brock, -8.2% Bryan.
      {"gpu_native_ps_texture_spec", "true"},
      // The Xenos multiply rule (0 x anything = 0) dropped in NATIVE pixel shaders only where both operands are proven
      // finite (assumes finite interpolators, constants and texture results). check_item ps_mul1: pictures 6 / 6,
      // hashes 467 / 467, gameplay identical.
      // Session 66 (Jarrod 2026-10-26, after the review packs: "the images look good"): mode 2 (also dropped where not
      // proven finite, with a NaN-to-0 fallback), implicit LOD and fused multiply-add are NATIVE defaults (looks right).
      {"gpu_native_ps_mul_mode", "2"},
      {"gpu_native_ps_implicit_lod", "true"},
      {"gpu_native_fma", "3"},
      // Session 66 (G1.1): a deferred texture-only resolve that a newer resolve into the same texture covers whole is
      // dropped (nothing sampled it: the batch would have been flushed). check_item g1_drop: pictures 6 / 6, hashes
      // 467 / 467, gameplay identical (4244-4249); A/B GPU -2.2 / +0.9 / -4.1% (Brock / Bryan / match).
      {"gpu_native_truth_drop_dead_resolves", "1"},
      // Session 66: the multiply rule in NATIVE's vertex shaders too (~40% of their instructions; -28% vertex work by
      // RGA), dropped only where both operands are proven finite, fetched float vertex data counted as finite.
      // check_item vs_mul1: pictures 6 / 6, hashes 467 / 467, gameplay identical (4251-4253).
      {"gpu_native_vs_mul_mode", "1"},
      {"gpu_native_vs_mul_fetch_finite", "true"},
      // Session 66 (native_ext): NATIVE pixel shaders without the alpha test / alpha-to-coverage / exponent bias /
      // gamma epilogue when the draw has none of them (~8% of pixel-shader work by RGA). check_item ps_epilogue: pictures
      // 6 / 6, hashes 467 / 467, gameplay identical (4287-4292); A/B within noise on the 4090.
      {"gpu_native_ps_epilogue_spec", "true"},
  };
}

inline std::vector<std::pair<std::string, std::string>> GraphicsFlags(const Choice& c, const std::string& dlss_dir,
                                                                      int monitor_hz) {
  std::vector<std::pair<std::string, std::string>> f;
  // An upscaler that picks the render and output size itself (DLSS, FSR quality / performance); DLAA and FSR native
  // anti-alias at the chosen resolution.
  // (Review fix: `dlss` is DLSS only; the FSR modes have their own block below.)
  const bool dlss = c.upscale >= 2 && c.upscale <= 4;
  const bool own_size = dlss || c.upscale == 6 || c.upscale == 7;
  if (c.original_look) f.push_back({"wwe2k14_preset_1440p60", "false"});
  else if (!own_size && c.resolution == 0) f.push_back({"resolution_scale", "1"});
  else if (!own_size && c.resolution == 2) f.push_back({"wwe2k14_preset_4k60", "true"});
  if ((c.frame_rate == 0 || c.frame_rate == 3) && !c.original_look) f.push_back({"wwe2k14_entrance_60fps", "false"});
  // Session 37: 30 everywhere for weak PCs (below 60 the game runs in slow motion: it steps a fixed 1/60 s). Matches
  // use the game's own 30 fps mode (step 1/30 s, run 2632: 33.3 ms frames, real time); entrances keep their stock 30
  // (their rate 60 = unchanged). The match clock counts updates, so it runs at 30 per second (frame-counted timers at
  // half rate: Jarrod to judge).
  if (c.frame_rate == 3) {
    f.push_back({"wwe2k14_step_hz", "30"});
    f.push_back({"wwe2k14_step_hz_entrance", "60"});
    // Session 41 (low-spec step 1, as --wwe2k14_preset_low): laptops, a shorter final spin before each vblank.
    f.push_back({"gpu_vblank_precise_timer_spin_us", "250"});
    // Session 44 (Jarrod, 2026-10-21, as --wwe2k14_preset_low): direct 1x texture loads for resolve outputs, exact,
    // less GPU time on both renderers (NATIVE and LIGHT); no effect above 720p (scaled textures have their own setting).
    f.push_back({"vulkan_texture_load_direct_unscaled", "true"});
  }
  const int hz = c.refresh_hz > 0 ? c.refresh_hz : monitor_hz;
  if (c.frame_rate == 2 && hz > 60) {  // game steps per second = the refresh rate; entrances at most 120
    f.push_back({"wwe2k14_step_hz", std::to_string(hz)});
    f.push_back({"wwe2k14_step_hz_entrance", std::to_string(std::min(hz, 120))});
    f.push_back({"wwe2k14_step_vblank", "true"});
    f.push_back({"vulkan_allow_present_mode_immediate", "false"});  // mailbox: every frame shown, no tearing
  }
  if (c.upscale == 1) f.push_back({"wwe2k14_dlaa", "true"});
  if (dlss) {
    static const char* modes[] = {"performance", "quality", "ultra_performance"};
    f.push_back({"wwe2k14_dlss", "true"});
    f.push_back({"wwe2k14_dlss_mode", modes[c.upscale - 2]});
  }
  if (c.texture_filter == 1 || c.texture_filter == 2)  // Session 33: anisotropic filtering 8x / 16x
    f.push_back({"wwe2k14_texture_filter", c.texture_filter == 2 ? "16" : "8"});
  if (IsFsr(c.upscale)) {  // Session 33
    static const char* modes[] = {"native", "quality", "performance"};
    f.push_back({"wwe2k14_fsr", "true"});
    f.push_back({"wwe2k14_fsr_mode", modes[c.upscale - 5]});
  }
  if (c.renderer == 1) f.push_back({"wwe2k14_light_renderer", "true"});  // Session 37
  if (c.renderer == 2)
    for (const auto& [name, value] : NativeRendererFlags()) f.push_back({name, value});
  // Session 54: NATIVE drawing below the whole size (no upscaler: DLAA / DLSS / FSR turn truth frames off; the SDK
  // logs and ignores a height it cannot draw).
  if (c.renderer == 2 && c.upscale == 0 && !c.original_look && ValidDrawHeight(c.resolution, c.draw_height))
    f.push_back({"gpu_native_truth_draw_height", std::to_string(c.draw_height)});
  // Wrestler motion vectors are always on with DLAA / DLSS / FSR (Session 28: the game's own default, no flag needed).
  if (IsNvidia(c.upscale) && !dlss_dir.empty()) f.push_back({"wwe2k14_dlaa_dir", dlss_dir});
  return f;
}

}  // namespace wwe2k14::gfxs
