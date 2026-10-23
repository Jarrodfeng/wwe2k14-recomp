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
#include <windows.h>
#include <shlobj.h>

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
  return version < 2 && renderer == 0 ? 2 : renderer;  // (Session 51's move: files from before version 2)
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
  return rtx;
}

// The main monitor's refresh rate.
inline int MonitorHz() {
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return int(dm.dmDisplayFrequency);
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
      // Session 54 (part B): on a card without fragment shader interlock (the host render target path) the truth
      // frames run too (no effect on cards with interlock). Forced fbo on the 4090: resolve hashes 467 / 467, parity
      // shots 6 / 6 byte-identical to NATIVE on interlock; vs LIGHT GPU -13.3% match, command thread -4.7%.
      {"gpu_native_truth_host_rt", "true"},
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
