// wwe2k14 - GRAPHICS menu in the game's own Options (Session 28). Item 1 (the entry in the list), item 2 (the screen).
//
// What this patches, at run time, in memory (nothing in the game's files; the strings are ours; docs\systems\menus.md):
//   OPTIONS = SmartMenu list menu 4 (flow records, menu 4 items 0..6). We build a flow table one record longer:
//     GRAPHICS = item 3, after GAMEPLAY, with the screen code of VISUAL PRESENTATION (1300).
//   The screen = the game's own VISUAL PRESENTATION frame ("Gameplay_Visual_Presentation", ctor sub_82B1B0C0, vtable
//     0x82062EE0): 7 rows of 16 bytes at this+48 {value, original, default, enabled}, the row-list widget at this+8.
//     Opened from GRAPHICS it is swapped to ours: labels and header through the text manager, values through the
//     frame's own value tables, three rows kept and four hidden, the settings read from / written to the launcher's
//     settings file (src\gfx_settings.h) instead of the game's save.
//   The text manager (object at [0x836F3A50], vtable slot 6 = sub_822563F8 GetString(id)) answers our ids.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "decode_probe.h"  // GuestPtr
#include "frame_stats.h"
#include "gfx_settings.h"
#include "gpu_pick.h"      // Session 37: the chosen card (no FSR on integrated graphics)
#include "input_script.h"  // g_request_quit (the clean quit)

namespace wwe2k14::gfx_menu {
// A "+dev" version string marks a development build (CMakeLists.txt); a release applies the launcher's settings to
// itself when it is started without the launcher.
constexpr bool IsDevBuild() {
#ifdef WWE2K14_VERSION
  return std::string_view(WWE2K14_VERSION).find("+dev") != std::string_view::npos;
#else
  return true;
#endif
}
}  // namespace wwe2k14::gfx_menu

REXCVAR_DEFINE_BOOL(wwe2k14_gfx_menu, true, "wwe2k14",
                    "Session 28: a GRAPHICS entry after GAMEPLAY in the main menu's OPTIONS list (a settings screen "
                    "in the game's own style). Default on (Jarrod, 2026-10-18); =false leaves the list stock. See "
                    "src/gfx_menu.h");
REXCVAR_DEFINE_BOOL(wwe2k14_quit_item, true, "wwe2k14",
                    "Session 28: a QUIT TO DESKTOP item at the end of the main menu's list (asks first, then closes the "
                    "game the way Alt+F4 does). Default on (Jarrod, 2026-10-18); =false leaves the list stock. See "
                    "src/gfx_menu.h");
REXCVAR_DEFINE_BOOL(wwe2k14_use_launcher_settings, !wwe2k14::gfx_menu::IsDevBuild(), "wwe2k14",
                    "Session 28: at start, apply the graphics settings in the launcher's settings.json (upscaling, "
                    "resolution, frame rate) to settings the command line left alone. Default: on in a release, off in "
                    "a +dev build (test runs would pick up the player's own file)");
REXCVAR_DEFINE_STRING(wwe2k14_settings_file, "", "wwe2k14",
                      "Session 28: the settings file the GRAPHICS menu and --wwe2k14_use_launcher_settings read and "
                      "write; empty = %APPDATA%\\wwe2k14-launcher\\settings.json (the launcher's)");
REXCVAR_DEFINE_INT32(wwe2k14_gfxmenu_msg_type, 3, "wwe2k14",
                     "Debug (Session 28): the message box type of the restart message (2 = YES / NO as the game's own "
                     "question; 3..5 tried in run 2108+)");
REXCVAR_DEFINE_INT32(wwe2k14_gfxmenu_msg_arg, 0, "wwe2k14",
                     "Debug (Session 28): the restart message event's word at +28 (the button layout for types 3 / 4)");
REXCVAR_DEFINE_BOOL(wwe2k14_gfxmenu_no_rtx, false, "wwe2k14",
                    "Debug (Session 28): pretend there is no RTX card (the GRAPHICS menu offers upscaling OFF only)");
REXCVAR_DEFINE_INT32(wwe2k14_gfxmenu_test_refuse, 0, "wwe2k14",
                     "Test (Session 28): 1 = expect an OPTIONS list of 6 items, 2 = expect another first label: the "
                     "flow table must be left stock and the reason logged (the safety rule of items 1 and 3)");
REXCVAR_DEFINE_BOOL(wwe2k14_gfxmenu_probe_mgr, false, "wwe2k14",
                    "Debug (Session 28): log every change of the message manager object's words (per frame)");
REXCVAR_DEFINE_BOOL(wwe2k14_gfxmenu_probe, false, "wwe2k14",
                    "Debug (Session 28): log the Options / VISUAL PRESENTATION screen's calls (text ids, frame "
                    "methods, SmartMenu fields). Log only. See src/gfx_menu.h");
REXCVAR_DEFINE_BOOL(wwe2k14_gfxmenu_probe_text_all, false, "wwe2k14",
                    "Debug (Session 28): with probe_text on, log every call (not only new ids) with its caller");
// Session 33 (item 2): TEXTURE FILTERING. The SDK forces 4x anisotropic filtering on every eligible texture by default
// (anisotropic_override 3); 8 / 16 raise it (anisotropic_override 4 / 5; it can change while the game runs).
REXCVAR_DEFINE_INT32(wwe2k14_texture_filter, 0, "wwe2k14",
                     "Session 33: anisotropic texture filtering: 0 = unchanged (the SDK's forced 4x), 8 = 8x, 16 = 16x "
                     "(sharper floors, ramps and mats at an angle). Sets anisotropic_override if left at default");
REXCVAR_DEFINE_BOOL(wwe2k14_gfxmenu_probe_text, false, "wwe2k14",
                    "Debug (Session 28): with the probe on, log every new text id the text manager is asked for "
                    "(set from a script with `cvar` right before the screen of interest)");

REX_EXTERN(__imp__sub_822563F8);
REX_EXTERN(__imp__sub_83424808);
REX_EXTERN(__imp__sub_82B1B0C0);
REX_EXTERN(__imp__sub_82B1B138);
REX_EXTERN(__imp__sub_82B1ADF8);
REX_EXTERN(__imp__sub_82B1B460);
REX_EXTERN(__imp__sub_82B1B2B8);
REX_EXTERN(__imp__sub_82B1B378);
REX_EXTERN(__imp__sub_82B1B1A8);
REX_EXTERN(__imp__sub_82B1AF78);
REX_EXTERN(__imp__sub_82B14FC0);
REX_EXTERN(__imp__sub_82B43BD0);
REX_EXTERN(__imp__sub_82841648);
REX_EXTERN(__imp__sub_82B07460);
REX_EXTERN(__imp__sub_82B074F0);
REX_EXTERN(__imp__sub_8291F5D8);
REX_EXTERN(sub_82B1ADF8);  // our own hook below: the row refresh with the swapped texts

namespace wwe2k14::gfx_menu {

// ---------------------------------------------------------------------------------------------------------------
// Guest helpers (guest words are big-endian in host memory)
// ---------------------------------------------------------------------------------------------------------------
inline uint32_t W(uint8_t* base, uint32_t addr) {
  if (addr < 0x10000u) return 0;
  const uint8_t* p = decode_probe::GuestPtr(base, addr);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t Frame() { return wwe2k14::g_guest_frame.load(std::memory_order_relaxed); }

inline std::string GuestStr(uint8_t* base, uint32_t addr, size_t max = 80) {
  if (addr < 0x10000u) return {};
  const char* p = reinterpret_cast<const char*>(decode_probe::GuestPtr(base, addr));
  size_t n = 0;
  while (n < max && p[n] != 0) ++n;
  std::string s(p, n);
  for (char& c : s)
    if (c < 32 || c > 126) c = '?';
  return s;
}

inline uint8_t* HostPtr(uint32_t guest) { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(guest); }
inline void Put32(uint32_t guest, uint32_t v) {
  uint8_t* p = HostPtr(guest);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
inline uint32_t AllocGuest(uint32_t bytes) { return REX_KERNEL_MEMORY()->SystemHeapAlloc(bytes); }
inline uint32_t AllocGuestString(const std::string& s) {
  const uint32_t a = AllocGuest(uint32_t(s.size() + 1));
  if (!a) return 0;
  std::memcpy(HostPtr(a), s.c_str(), s.size() + 1);
  return a;
}

// Calls a guest function (host symbol) with up to 6 word arguments on the current guest thread; returns r3; the
// registers are restored.
inline uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t a0 = 0,
                          uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0, uint32_t a5 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = a0, ctx.r4.u64 = a1, ctx.r5.u64 = a2, ctx.r6.u64 = a3, ctx.r7.u64 = a4, ctx.r8.u64 = a5;
  ctx.lr = 0;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

constexpr uint32_t kSmartMenuPtr = 0x8392945C;

inline std::string SmartMenuSummary(uint8_t* base) {
  const uint32_t sm = W(base, kSmartMenuPtr);
  if (!sm) return "sm=null";
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "sm=%08X menu=%d cursor=%d target=%d ctx=%d depth=[%d,%d] prev=[%d,%d] hist=[%d,%d,%d,%d] mode=%d",
                sm, int32_t(W(base, sm + 364)), int32_t(W(base, sm + 352)), int32_t(W(base, sm + 368)),
                int32_t(W(base, sm + 268)), int32_t(W(base, sm + 272)), int32_t(W(base, sm + 276)),
                int32_t(W(base, sm + 356)), int32_t(W(base, sm + 360)), int32_t(W(base, sm + 280)),
                int32_t(W(base, sm + 284)), int32_t(W(base, sm + 288)), int32_t(W(base, sm + 292)),
                int32_t(W(base, W(base, sm + 11412) + 24)));
  return buf;
}

inline std::mutex g_text_mutex;
inline std::set<uint32_t> g_text_seen;

inline void DumpObject(uint8_t* base, const char* what, uint32_t obj, uint32_t bytes) {
  for (uint32_t off = 0; off < bytes; off += 32) {
    char line[300];
    int n = std::snprintf(line, sizeof(line), "[wwe2k14][gfxmenu] %s +%03u:", what, off);
    for (uint32_t k = 0; k < 8 && off + k * 4 < bytes; ++k)
      n += std::snprintf(line + n, sizeof(line) - n, " %08X", W(base, obj + off + k * 4));
    REXLOG_INFO("{}", line);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The graphics settings: what this run is doing, what the file says, startup apply.
// ---------------------------------------------------------------------------------------------------------------
inline std::filesystem::path SettingsFile() {
  const std::string f = REXCVAR_GET(wwe2k14_settings_file);
  return f.empty() ? gfxs::DefaultSettingsPath() : std::filesystem::path(f);
}

inline bool FlagIsTrue(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}
inline int FlagInt(const char* name) { return std::atoi(rex::cvar::GetFlagByName(name).c_str()); }

// Session 33: --wwe2k14_texture_filter -> the SDK's anisotropic_override (only if left at default).
inline const char* AnisoForFilter(int f) { return f >= 16 ? "5" : f >= 8 ? "4" : nullptr; }
inline void ConfigureTextureFilter() {
  const char* v = AnisoForFilter(REXCVAR_GET(wwe2k14_texture_filter));
  if (v && rex::cvar::GetFlagSource("anisotropic_override") == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName("anisotropic_override", v);
  if (v) REXLOG_INFO("[wwe2k14][gfx] texture filtering {}x (anisotropic_override {})",
                     REXCVAR_GET(wwe2k14_texture_filter), rex::cvar::GetFlagByName("anisotropic_override"));
}

inline gfxs::Choice g_running;       // what this run does now (after the startup apply and live changes)
inline bool g_streamline_running = false;  // the NVIDIA runtime was loaded at start (DLAA can switch on and off live)
inline std::string g_dlss_dir;       // the runtime folder (found at start)

inline bool g_force_scale_one = false;

// Before the presets (OnPreSetup): apply the settings file to the settings the command line left at default.
inline void ApplyLauncherSettings() {
  if (!REXCVAR_GET(wwe2k14_use_launcher_settings)) return;
  const std::filesystem::path path = SettingsFile();
  const std::string json = gfxs::ReadFile(path);
  // Session 51 (Jarrod, 2026-10-22: "NATIVE for everyone"): no file = the launcher's defaults, so the NATIVE renderer
  // (the other defaults set nothing: 1440p, 60 fps, no upscaler are the game's own).
  if (json.empty())
    REXLOG_INFO("[wwe2k14][gfxmenu] no settings file at {}: the launcher's defaults (NATIVE renderer)",
                gfxs::Utf8(path));
  gfxs::Choice c;
  gfxs::Parse(json, c);
  const std::string dir = gfxs::StreamlineDir(rex::cvar::GetFlagByName("wwe2k14_dlaa_dir"), c.dlss_dir);
  if (gfxs::IsFsr(c.upscale) && !gfxs::HasFsrRuntime()) {  // Session 33
    REXLOG_WARN("[wwe2k14][gfxmenu] settings ask for FSR ({}) but amd_fidelityfx_vk.dll is not next to the game: "
                "upscaling off", c.upscale);
    c.upscale = 0;
  }
  if (gfxs::IsNvidia(c.upscale) && (dir.empty() || REXCVAR_GET(wwe2k14_gfxmenu_no_rtx) || !gfxs::HasRtxCard())) {
    REXLOG_WARN("[wwe2k14][gfxmenu] settings ask for upscaling {} but there is no RTX card or no NVIDIA runtime "
                "(folder '{}'): upscaling off",
                c.upscale, dir);
    c.upscale = 0;
  }
  // A command line that picks a preset or an upscaler itself is not mixed with the file's resolution (review fix:
  // the file's 4K or 720p would otherwise be applied over the command line's own choice).
  auto cmdline = [](const char* n) { return rex::cvar::GetFlagSource(n) != rex::cvar::Source::kDefault; };
  const bool own_size = cmdline("wwe2k14_preset_1440p60") || cmdline("wwe2k14_preset_4k60") || cmdline("wwe2k14_dlss") ||
                        cmdline("wwe2k14_dlaa") || cmdline("wwe2k14_fsr") || cmdline("resolution_scale");
  std::string applied;
  for (const auto& [name, value] : gfxs::GraphicsFlags(c, dir, gfxs::MonitorHz())) {
    if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) continue;  // the command line wins
    if (own_size && (name == "resolution_scale" || name == "wwe2k14_preset_4k60" || name == "wwe2k14_preset_1440p60"))
      continue;
    rex::cvar::SetFlagByName(name, value);
    applied += (applied.empty() ? "" : " ") + name + "=" + value;
    // Run 2139: with the file's 720p (resolution_scale 1, the compiled-in default) the game still came up at 1440p
    // although the value was set before the presets ran; why is not confirmed (the SDK marks a flag set here as set
    // at run time, so the preset's "only if left at default" check should have left it alone). It is set again once
    // every start-up setting is in place (CaptureRunning); run 2141 confirmed the result.
    if (name == "resolution_scale") g_force_scale_one = true;
  }
  REXLOG_INFO("[wwe2k14][gfxmenu] settings file {}: upscale {}, resolution {}, frame rate {}, renderer {} -> {}",
              gfxs::Utf8(path), c.upscale, c.resolution, c.frame_rate, c.renderer,
              applied.empty() ? "(nothing to set)" : applied);
}

inline bool DlssPossible();  // below

// After every start-up setting is in place (end of OnPreSetup): what this run is doing, read back from the settings.
inline void CaptureRunning() {
  if (g_force_scale_one && rex::cvar::GetFlagSource("resolution_scale") != rex::cvar::Source::kCommandLine)
    rex::cvar::SetFlagByName("resolution_scale", "1");  // 720p from the settings file (see ApplyLauncherSettings)
  gfxs::Choice r;
  const std::string mode = rex::cvar::GetFlagByName("wwe2k14_dlss_mode");
  const std::string fsr_mode = rex::cvar::GetFlagByName("wwe2k14_fsr_mode");
  if (FlagIsTrue("wwe2k14_fsr")) r.upscale = fsr_mode == "native" ? 5 : fsr_mode == "performance" ? 7 : 6;
  else if (FlagIsTrue("wwe2k14_dlss")) r.upscale = mode == "quality" ? 3 : mode == "ultra_performance" ? 4 : 2;
  else if (FlagIsTrue("wwe2k14_dlaa")) r.upscale = 1;
  const std::string scale = rex::cvar::GetFlagByName("resolution_scale");
  r.resolution = scale == "1" ? 0 : scale == "3" ? 2 : 1;
  r.texture_filter = REXCVAR_GET(wwe2k14_texture_filter) >= 16 ? 2 : REXCVAR_GET(wwe2k14_texture_filter) >= 8 ? 1 : 0;
  if (FlagInt("wwe2k14_step_hz") > 60) r.frame_rate = 2, r.refresh_hz = int(FlagInt("wwe2k14_step_hz"));
  else if (!FlagIsTrue("wwe2k14_entrance_60fps")) r.frame_rate = 0;
  else r.frame_rate = 1;
  g_running = r;
  g_dlss_dir = rex::cvar::GetFlagByName("vulkan_streamline_dir");
  g_streamline_running = !g_dlss_dir.empty() && (FlagIsTrue("wwe2k14_dlaa") || FlagIsTrue("wwe2k14_dlss"));
  REXLOG_INFO("[wwe2k14][gfxmenu] running: upscale {}, resolution {}, frame rate {}, NVIDIA runtime '{}' ({})",
              r.upscale, r.resolution, r.frame_rate, g_dlss_dir, g_streamline_running ? "loaded" : "not loaded");
  // The RTX / runtime question once now (DXGI takes a moment), not the first time the GRAPHICS screen opens.
  if (REXCVAR_GET(wwe2k14_gfx_menu))
    REXLOG_INFO("[wwe2k14][gfxmenu] upscaling offered in the menu: NVIDIA {}, FSR {}", DlssPossible() ? "yes" : "no",
                gfxs::HasFsrRuntime() ? "yes" : "no (amd_fidelityfx_vk.dll missing)");
}

// DLAA / DLSS can be offered: an RTX card and the NVIDIA runtime (the one this run loaded, or one found next to the
// game or named in the settings file).
inline bool DlssPossible() {
  static int cached = -1;
  if (cached >= 0) return cached != 0;
  std::string file_dir;
  gfxs::GetString(gfxs::ReadFile(SettingsFile()), "dlss_dir", file_dir);
  const bool dir = g_streamline_running || !gfxs::StreamlineDir(rex::cvar::GetFlagByName("wwe2k14_dlaa_dir"), file_dir).empty();
  cached = (dir && !REXCVAR_GET(wwe2k14_gfxmenu_no_rtx) && gfxs::HasRtxCard()) ? 1 : 0;
  return cached != 0;
}

// The three rows of the screen: the menu's value index for each. Session 31: UPSCALING in the launcher's order (OFF,
// DLAA, then DLSS ultra performance, performance, quality); FRAME RATE 60 / 120 / UNCAPPED (the launcher's
// FrameRateRow): 60 = frame_rate 1, 120 = high refresh (2) at 120 steps, UNCAPPED = high refresh at the monitor's rate
// (refresh_hz 0). The row has the game's three value slots, so CLASSIC (frame_rate 0, the launcher can still keep it)
// has none: the row shows 60 and the file keeps CLASSIC unless the row is changed.
// Session 33: FSR NATIVE / QUALITY / PERFORMANCE after the NVIDIA modes, on every card with AMD's runtime next to the
// game: g_up_order = menu index -> the settings file's upscale (RTX: OFF, DLAA, DLSS x3, FSR x3; else OFF, FSR x3).
inline int g_up_order[8] = {0, 1, 4, 2, 3, 5, 6, 7};
inline int g_up_count = 1;
// Session 33: a fourth row, TEXTURE FILTERING (STANDARD / 8X / 16X), in the stock screen's row 3 slot.
constexpr int kRows = 4;
inline int g_vals[kRows] = {0, 1, 1, 0};           // the screen's current value index per row
inline int g_counts[kRows] = {1, 3, 3, 3};         // how many values each row offers
inline int g_frame_loaded = 0;                     // FRAME RATE's index when the screen opened
inline gfxs::Choice g_frame_file;                  // and the frame_rate / refresh_hz it stood for

inline int UpscaleToIndex(int upscale) {
  for (int i = 0; i < g_up_count; ++i)
    if (g_up_order[i] == upscale) return i;
  return 0;
}
inline void BuildUpscaleOrder(bool nvidia, bool fsr) {
  g_up_count = 0;
  g_up_order[g_up_count++] = 0;
  if (nvidia)
    for (int v : {1, 4, 2, 3}) g_up_order[g_up_count++] = v;
  if (fsr)
    for (int v : {5, 6, 7}) g_up_order[g_up_count++] = v;
}
inline int FrameRateIndex(const gfxs::Choice& c) { return c.frame_rate == 2 ? (c.refresh_hz == 120 ? 1 : 2) : 0; }

// Session 37 (FSR never breaks a game): FSR in the menu only with AMD's runtime, not on integrated graphics (FSR 3.1
// does not support them; a UHD 620 shook and hung) and not after it failed in this run (src\upscale_guard.h).
inline bool FsrOffered() {
  if (!gfxs::HasFsrRuntime()) return false;
  if (gpu_pick::g_known && gpu_pick::g_chosen.integrated()) return false;
  return rex::cvar::GetFlagByName("gpu_fsr_failed").empty();
}

// The values the screen starts with: the settings file where it has them, else what the game is running.
inline void LoadScreenValues() {
  gfxs::Choice c = g_running;
  const std::string json = gfxs::ReadFile(SettingsFile());
  if (!json.empty()) {
    gfxs::Choice f = c;
    const gfxs::Present p = gfxs::Parse(json, f);
    if (p.upscale) c.upscale = f.upscale;
    if (p.resolution) c.resolution = f.resolution;
    if (p.frame_rate) c.frame_rate = f.frame_rate;
    if (p.refresh_hz) c.refresh_hz = f.refresh_hz;
    if (p.texture_filter) c.texture_filter = f.texture_filter;
  }
  const bool dlss = DlssPossible(), fsr = FsrOffered();
  BuildUpscaleOrder(dlss, fsr);
  if ((!dlss && gfxs::IsNvidia(c.upscale)) || (!fsr && gfxs::IsFsr(c.upscale))) c.upscale = 0;
  g_counts[0] = g_up_count, g_counts[1] = 3, g_counts[2] = 3, g_counts[3] = 3;
  g_vals[0] = UpscaleToIndex(c.upscale), g_vals[1] = FrameRateIndex(c), g_vals[2] = c.resolution;
  g_vals[3] = std::clamp(c.texture_filter, 0, 2);
  g_frame_loaded = g_vals[1], g_frame_file = c;
}

// ACCEPT: write the three choices to the settings file, apply what can change live, say whether the rest needs a
// restart. Live: DLAA <-> off when the NVIDIA runtime is loaded; classic <-> 60 everywhere (the entrance hack is read
// as it is used). Everything else (a DLSS mode, the render resolution, high refresh) is read at start.
inline bool CommitScreenValues() {
  gfxs::Choice n = g_running;
  n.upscale = g_up_order[std::clamp(g_vals[0], 0, g_up_count - 1)];
  if (g_vals[1] == g_frame_loaded) {  // untouched: keep exactly what the file had (CLASSIC included)
    n.frame_rate = g_frame_file.frame_rate, n.refresh_hz = g_frame_file.refresh_hz;
  } else {
    const int fi = std::clamp(g_vals[1], 0, 2);
    n.frame_rate = fi == 0 ? 1 : 2, n.refresh_hz = fi == 1 ? 120 : 0;
  }
  n.resolution = std::clamp(g_vals[2], 0, 2);
  n.texture_filter = std::clamp(g_vals[3], 0, 2);
  const std::filesystem::path path = SettingsFile();
  bool cleared_original_look = false;
  const bool saved = gfxs::Save(path, n, &cleared_original_look);
  // HIGH REFRESH on a monitor of 60 Hz or less runs as 60 everywhere (the game's mapping): compare what will run.
  gfxs::Choice file_choice;
  gfxs::Parse(gfxs::ReadFile(path), file_choice);
  const int hz = file_choice.refresh_hz > 0 ? file_choice.refresh_hz : gfxs::MonitorHz();
  gfxs::Choice eff = n;  // what the next start will run
  if (eff.frame_rate == 2 && hz <= 60) eff.frame_rate = 1;
  if (eff.upscale != g_running.upscale && g_streamline_running && g_running.upscale <= 1 && eff.upscale <= 1) {
    const char* on = eff.upscale == 1 ? "true" : "false";
    rex::cvar::SetFlagByName("gpu_dlaa", on);
    rex::cvar::SetFlagByName("gpu_dlaa_jitter", on);
    g_running.upscale = eff.upscale;
  }
  if (eff.frame_rate != g_running.frame_rate && g_running.frame_rate <= 1 && eff.frame_rate <= 1) {
    rex::cvar::SetFlagByName("wwe2k14_entrance_60fps", eff.frame_rate == 1 ? "true" : "false");
    g_running.frame_rate = eff.frame_rate;
  }
  // DLSS and FSR quality / performance pick the render size themselves (the resolution row does not matter then).
  // Session 33: texture filtering applies at once (the SDK's anisotropic_override is read per sampler).
  if (eff.texture_filter != g_running.texture_filter) {
    const char* v = AnisoForFilter(eff.texture_filter == 2 ? 16 : eff.texture_filter == 1 ? 8 : 0);
    rex::cvar::SetFlagByName("anisotropic_override", v ? v : "3");
    g_running.texture_filter = eff.texture_filter;
  }
  const bool dlss_running = (g_running.upscale >= 2 && g_running.upscale <= 4) || g_running.upscale >= 6;
  // Session 31: 120 <-> UNCAPPED (both high refresh) differ in the step rate, read at start.
  const bool step_changed = eff.frame_rate == 2 && g_running.frame_rate == 2 && hz != g_running.refresh_hz;
  const bool restart = saved && (eff.upscale != g_running.upscale || eff.frame_rate != g_running.frame_rate ||
                                 step_changed || (!dlss_running && eff.resolution != g_running.resolution));
  REXLOG_INFO("[wwe2k14][gfxmenu] ACCEPT: upscale {}, resolution {}, frame rate {} saved to {} ({}){}; now running "
              "upscale {}, frame rate {}; restart needed: {}",
              n.upscale, n.resolution, n.frame_rate, gfxs::Utf8(path), saved ? "ok" : "FAILED",
              cleared_original_look ? " (the file's \"original look\" is switched off so these apply)" : "",
              g_running.upscale, g_running.frame_rate, restart ? "yes" : "no");
  return restart;
}

// Calls a guest function by its guest address (an indirect call, as the generated code does for a virtual method).
inline uint32_t CallGuestAddr(PPCContext& ctx, uint8_t* base, uint32_t addr, uint32_t a0 = 0, uint32_t a1 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = a0, ctx.r4.u64 = a1;
  ctx.lr = 0;
  REX_CALL_INDIRECT_FUNC(addr);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

// The game's own message box: the SmartMenu's message manager ([0x839294BC], vtable slot 27) takes an event
// {type, text id, ...} (initialised by sub_8291F5D8); the same call the stock screen makes for "Are you sure you
// don't want to keep these settings?" (type 2 = YES / NO). Run 2085: the box asks the text manager for its message.
constexpr uint32_t kMessageManagerPtr = 0x839294BC;
inline uint32_t g_event_buf = 0;
inline bool PostMessageBox(PPCContext& ctx, uint8_t* base, uint32_t type, uint32_t text_id, uint32_t arg28 = 0) {
  const uint32_t mgr = W(base, kMessageManagerPtr);
  if (!mgr) return false;
  // The game's own post routine (sub_8291FCA0) takes an event only while the manager is idle (state 0 or 5) and drops
  // it silently otherwise (another box is up or opening): nothing is posted then, and the caller must know (review).
  const uint32_t state = W(base, mgr + 16);
  if (state != 0 && state != 5) return false;
  if (!g_event_buf) g_event_buf = AllocGuest(64);
  if (!g_event_buf) return false;
  CallGuest(ctx, base, __imp__sub_8291F5D8, g_event_buf);
  Put32(g_event_buf + 0, type);
  Put32(g_event_buf + 4, text_id);
  Put32(g_event_buf + 28, arg28);
  CallGuestAddr(ctx, base, W(base, W(base, mgr) + 108), mgr, g_event_buf);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Our text ids. The text manager (sub_822563F8) is asked for ids below ~100000; ours sit far above that and are
// answered from guest strings we allocate (the game keeps the pointer, so each string lives for the whole run).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kTextBase = 0x7C000000u;
constexpr uint32_t kTextGraphics = kTextBase + 1;   // the OPTIONS list label and the screen's title
constexpr uint32_t kTextPcSettings = kTextBase + 2;  // the screen's sub-title
constexpr uint32_t kTextLabel[4] = {kTextBase + 3, kTextBase + 4, kTextBase + 5, kTextBase + 7};
constexpr uint32_t kTextVal0 = kTextBase + 0x10, kTextVal1 = kTextBase + 0x20, kTextVal2 = kTextBase + 0x30;
constexpr uint32_t kTextVal3 = kTextBase + 0x50;  // Session 33: TEXTURE FILTERING's values
constexpr uint32_t kTextRestart = kTextBase + 0x40;
constexpr uint32_t kTextQuit = kTextBase + 6;        // the main menu list label
constexpr uint32_t kTextQuitAsk = kTextBase + 0x41;  // its confirmation question
constexpr uint32_t kTextPhoto = kTextBase + 0x60;    // Session 33: PHOTO MODE in the pause menu (src/photo_mode.h)
inline bool g_photo_pause_item = false;              // set by photo_mode.h when that row is in use

struct TextEntry {
  uint32_t id;
  const char* text;
  uint32_t guest;
};
inline std::mutex g_str_mutex;
inline TextEntry g_texts[] = {
    {kTextGraphics, "GRAPHICS", 0},
    {kTextPcSettings, "PC SETTINGS", 0},
    {kTextLabel[0], "UPSCALING", 0},
    {kTextLabel[1], "FRAME RATE", 0},
    {kTextLabel[2], "RESOLUTION", 0},
    {kTextLabel[3], "TEXTURE FILTERING", 0},
    {kTextVal3 + 0, "STANDARD", 0},
    {kTextVal3 + 1, "8X", 0},
    {kTextVal3 + 2, "16X", 0},
    // Session 31 (Jarrod: the launcher and this menu match): UPSCALING in the launcher's order, FRAME RATE 60 / 120 /
    // UNCAPPED (g_up_order, FrameRateIndex below). Session 33: UPSCALING's texts by the settings file's value.
    {kTextVal0 + 0, "OFF", 0},
    {kTextVal0 + 1, "DLAA", 0},
    {kTextVal0 + 2, "DLSS PERFORMANCE", 0},
    {kTextVal0 + 3, "DLSS QUALITY", 0},
    {kTextVal0 + 4, "DLSS ULTRA PERF", 0},
    {kTextVal0 + 5, "FSR NATIVE", 0},
    {kTextVal0 + 6, "FSR QUALITY", 0},
    {kTextVal0 + 7, "FSR PERFORMANCE", 0},
    {kTextVal1 + 0, "60", 0},
    {kTextVal1 + 1, "120", 0},
    {kTextVal1 + 2, "UNCAPPED", 0},
    {kTextVal2 + 0, "720P", 0},
    {kTextVal2 + 1, "1440P", 0},
    {kTextVal2 + 2, "4K", 0},
    {kTextRestart, "Some changes apply the next time you start the game.", 0},
    {kTextQuit, "QUIT TO DESKTOP", 0},
    {kTextQuitAsk, "Quit to desktop?", 0},
    {kTextPhoto, "PHOTO MODE", 0},
};

inline uint32_t TextFor(uint32_t id) {
  std::lock_guard<std::mutex> lock(g_str_mutex);
  for (TextEntry& t : g_texts) {
    if (t.id != id) continue;
    if (!t.guest) t.guest = AllocGuestString(t.text);
    return t.guest;
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Items 1 and 3: GRAPHICS after GAMEPLAY in the OPTIONS list (flow menu 4), QUIT TO DESKTOP after SHOP in the main
// menu's list (flow menu 1).
//
// The list menus are rows of a loaded "menu flow" table (the SmartMenu's [0x839294DC] holder; every lookup goes
// through sub_83424808(T, menu, item)). T = holder data + 4:
//   T+0   record count      T+4   record array (76-byte records)
//   T+84  index entry count T+88  index array (12-byte entries {first record u32, record count u16, ...}, by menu id)
// Record words: +0 record id, +4 parent record id, +8 menu id, +12 (u16 item index << 16 | flags), +24 label text
// id, +40 flags (0x10 = screen code), +44 (+ +52) the screen code. (Session 28 dump of run 2081, menus 1, 4 and 20.)
//
// We build a new record array with the extra records and a new index, then point T at them. Nothing in the game's
// own data is modified; a menu that does not look as expected (another version, another length) is left alone and
// the reason logged. Each new record starts as a copy of a neighbour in its menu (a screen entry), with its own
// record id, label and screen code; the items after it move down one.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kRecSize = 76;
constexpr uint32_t kMainMenu = 1, kOptionsMenu = 4;
constexpr uint32_t kMainLabels[8] = {0xA029, 0xA02B, 0xA0D1, 0xA02D, 0xA02E, 0xA02A, 0xA030, 0xA04B};
constexpr uint32_t kOptionsLabels[7] = {0xA0E0, 0xA04C, 0xA04D, 0xA04E, 0xA04F, 0xA0F1, 0xA0F3};
constexpr uint32_t kGraphicsRecordId = 0x00001C00, kQuitRecordId = 0x00001C01;  // far above the game's ids (< 0x200)
constexpr uint32_t kScreenCodeVP = 1300;
constexpr uint32_t kQuitCode = 1599;  // no screen of the game has it: the item is answered by our hook (item 3)
constexpr uint32_t kQuitItem = 8;     // QUIT TO DESKTOP's place in the main menu list (after SHOP, item 7)

// The game frees a flow table's two arrays itself when it destroys the table (sub_83425C68, reached from the holder's
// destructor sub_8293EBD8 whenever a match, Create-a-Wrestler or the story takes over), with its own allocator, which
// must never see ours (review, Session 28). So each patched table is remembered with the game's own arrays, and the
// destructor hook (below) puts those back before the game frees them; ours are freed by us.
struct FlowPatch {
  uint32_t T, orig_count, orig_recs, orig_idx, new_recs, new_idx;
  bool has_quit;
};
inline std::mutex g_flow_mutex;
inline std::vector<FlowPatch> g_flow_live;                     // the tables patched now
inline std::vector<std::pair<uint32_t, uint32_t>> g_flow_refused_tables;  // (T, record array) refused: not looked at again
inline int g_flow_patches = 0, g_flow_refused = 0;             // for the log

struct Insertion {
  uint32_t menu, at, n_expected;
  const uint32_t* expected;  // the labels the menu must have (text ids, in order)
  uint32_t record_id, label, code;
  const char* name;
};

inline bool EnsureTablesWritable();  // below: the GRAPHICS screen's value tables (a failure leaves the entry out)

inline std::vector<Insertion> WantedInsertions() {
  std::vector<Insertion> v;
  static const uint32_t kWrongLabels[7] = {0xA0E1, 0xA04C, 0xA04D, 0xA04E, 0xA04F, 0xA0F1, 0xA0F3};
  const int test = REXCVAR_GET(wwe2k14_gfxmenu_test_refuse);
  if (REXCVAR_GET(wwe2k14_gfx_menu) && EnsureTablesWritable())
    v.push_back({kOptionsMenu, 3, test == 1 ? 6u : 7u, test == 2 ? kWrongLabels : kOptionsLabels, kGraphicsRecordId,
                 kTextGraphics, kScreenCodeVP, "GRAPHICS after GAMEPLAY in OPTIONS"});
  if (REXCVAR_GET(wwe2k14_quit_item))
    v.push_back({kMainMenu, kQuitItem, 8, kMainLabels, kQuitRecordId, kTextQuit, kQuitCode,
                 "QUIT TO DESKTOP after SHOP in the main menu"});
  return v;
}

inline void PutBe(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
inline uint32_t GetBe(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline bool PatchFlowTable(uint8_t* base, uint32_t T, const std::vector<Insertion>& wanted) {
  const uint32_t count = W(base, T), recs = W(base, T + 4), idx_count = W(base, T + 84), idx = W(base, T + 88);
  auto refuse = [&](const char* why, const char* name = "") {
    if (g_flow_refused++ < 8) REXLOG_WARN("[wwe2k14][gfxmenu] flow table {:08X} left stock ({}): {}", T, name, why);
    if (g_flow_refused_tables.size() >= 32) g_flow_refused_tables.erase(g_flow_refused_tables.begin());
    g_flow_refused_tables.push_back({T, recs});  // this table (these arrays) is not checked again
    return false;
  };
  if (!recs || !idx) return refuse("no records or index");
  if (count < 50 || count > 4000) return refuse("record count out of range");
  if (idx_count < 8 || idx_count > 200) return refuse("index count out of range");
  for (uint32_t m = 0; m < idx_count; ++m)
    if (W(base, idx + 12 * m) + (W(base, idx + 12 * m + 4) >> 16) > count)
      return refuse("an index entry reaches past the record array");
  struct Pos {
    uint32_t pos;
    const Insertion* in;
    uint32_t first, n;
  };
  std::vector<Pos> positions;
  for (const Insertion& in : wanted) {
    if (in.menu >= idx_count) return refuse("menu not in the index", in.name);
    const uint32_t first = W(base, idx + 12 * in.menu), n = W(base, idx + 12 * in.menu + 4) >> 16;
    if (n != in.n_expected || in.at > n) return refuse("the menu does not have the expected number of items", in.name);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t r = recs + (first + i) * kRecSize;
      if (int32_t(W(base, r + 8)) != int32_t(in.menu) || (W(base, r + 12) >> 16) != i ||
          W(base, r + 24) != in.expected[i])
        return refuse("a record does not match the expected list", in.name);
    }
    positions.push_back({first + in.at, &in, first, n});
  }
  if (positions.empty()) return false;
  std::stable_sort(positions.begin(), positions.end(), [](const Pos& a, const Pos& b) { return a.pos < b.pos; });
  const uint32_t added = uint32_t(positions.size());
  std::vector<uint8_t> out(size_t(count + added) * kRecSize);
  const uint8_t* src = HostPtr(recs);
  size_t pi = 0;
  uint32_t o = 0;
  for (uint32_t i = 0; i <= count; ++i) {
    while (pi < positions.size() && positions[pi].pos == i) {
      const Pos& p = positions[pi++];
      const uint32_t tmpl = p.in->at < p.n ? i : i - 1;  // modelled on the record it pushes down, or on the last one
      uint8_t* r = out.data() + size_t(o++) * kRecSize;
      std::memcpy(r, src + size_t(tmpl) * kRecSize, kRecSize);
      PutBe(r + 0, p.in->record_id);
      PutBe(r + 12, (p.in->at << 16) | (GetBe(r + 12) & 0xFFFF));
      PutBe(r + 24, p.in->label);
      PutBe(r + 44, p.in->code);
      PutBe(r + 52, 0);
    }
    if (i == count) break;
    uint8_t* r = out.data() + size_t(o++) * kRecSize;
    std::memcpy(r, src + size_t(i) * kRecSize, kRecSize);
    for (const Pos& p : positions) {  // the items at and after the new one move down (item index = high half of +12)
      if (i >= p.first && i < p.first + p.n && (i - p.first) >= p.in->at)
        PutBe(r + 12, (((GetBe(r + 12) >> 16) + 1) << 16) | (GetBe(r + 12) & 0xFFFF));
    }
  }
  std::vector<uint8_t> new_idx(size_t(idx_count) * 12);
  std::memcpy(new_idx.data(), HostPtr(idx), new_idx.size());
  for (uint32_t m = 0; m < idx_count; ++m) {
    uint8_t* e = new_idx.data() + size_t(m) * 12;
    const uint32_t f = GetBe(e), c = GetBe(e + 4) >> 16;
    if (!c) continue;
    uint32_t shift = 0, own = 0;
    for (const Pos& p : positions) {
      // a record put in front of this menu's first moves it down, unless it is this menu's own (item 0)
      if (p.pos < f || (p.pos == f && p.in->menu != m)) ++shift;
      if (p.in->menu == m) ++own;
    }
    PutBe(e, f + shift);
    PutBe(e + 4, ((c + own) << 16) | (GetBe(e + 4) & 0xFFFF));
  }
  const uint32_t new_recs = AllocGuest(uint32_t(out.size())), new_idx_g = AllocGuest(uint32_t(new_idx.size()));
  if (!new_recs || !new_idx_g) return refuse("guest allocation failed");
  std::memcpy(HostPtr(new_recs), out.data(), out.size());
  std::memcpy(HostPtr(new_idx_g), new_idx.data(), new_idx.size());
  bool has_quit = false;
  for (const Insertion& in : wanted) has_quit |= in.record_id == kQuitRecordId;
  g_flow_live.push_back({T, count, recs, idx, new_recs, new_idx_g, has_quit});
  Put32(T + 0, count + added);
  Put32(T + 4, new_recs);
  Put32(T + 88, new_idx_g);
  ++g_flow_patches;
  std::string names;
  for (const Insertion& in : wanted) names += (names.empty() ? "" : "; ") + std::string(in.name);
  REXLOG_INFO("[wwe2k14][gfxmenu] menu flow table {:08X}: {} -> {} records (new array {:08X}, index {:08X}; patch #{}): {}",
              T, count, count + added, new_recs, new_idx_g, g_flow_patches, names);
  return true;
}

// Called from the lookup hook with the table pointer (r3 of sub_83424808).
inline void EnsureFlowPatched(uint8_t* base, uint32_t T) {
  if (T < 0x10000u) return;
  std::lock_guard<std::mutex> lock(g_flow_mutex);
  const uint32_t recs = W(base, T + 4);
  if (!recs) return;  // not loaded (or already taken apart)
  for (size_t i = 0; i < g_flow_live.size(); ++i) {
    if (g_flow_live[i].T != T) continue;
    if (g_flow_live[i].new_recs == recs) return;  // already ours
    // The game put arrays of its own into this table without destroying it (a reload): ours are no longer
    // referenced; they are left allocated (who owns them now is not known) and the table is looked at afresh.
    REXLOG_WARN("[wwe2k14][gfxmenu] flow table {:08X} was reloaded by the game (arrays {:08X} -> {:08X})", T,
                g_flow_live[i].new_recs, recs);
    g_flow_live.erase(g_flow_live.begin() + i);
    break;
  }
  for (const auto& [rt, rr] : g_flow_refused_tables)
    if (rt == T && rr == recs) return;  // checked before, left stock
  const std::vector<Insertion> wanted = WantedInsertions();
  if (!wanted.empty()) PatchFlowTable(base, T, wanted);
}

// The game is about to destroy flow table T (sub_83425C68): put its own arrays back so that it frees those, and free ours.
inline void ReleaseFlowPatch(uint8_t* base, uint32_t T) {
  std::lock_guard<std::mutex> lock(g_flow_mutex);
  for (size_t i = 0; i < g_flow_live.size(); ++i) {
    if (g_flow_live[i].T != T) continue;
    const FlowPatch p = g_flow_live[i];
    g_flow_live.erase(g_flow_live.begin() + i);
    // each array only when the table still holds ours (the game may have replaced one itself)
    if (W(base, T + 4) == p.new_recs) {
      Put32(T + 0, p.orig_count);
      Put32(T + 4, p.orig_recs);
      REX_KERNEL_MEMORY()->SystemHeapFree(p.new_recs);
    }
    if (W(base, T + 88) == p.new_idx) {
      Put32(T + 88, p.orig_idx);
      REX_KERNEL_MEMORY()->SystemHeapFree(p.new_idx);
    }
    REXLOG_INFO("[wwe2k14][gfxmenu] flow table {:08X} taken apart by the game: its own arrays put back, ours freed", T);
    return;
  }
}

// A table with QUIT TO DESKTOP in it is in use (the item's answer is only ours while the patch is).
inline bool QuitPatchLive() {
  std::lock_guard<std::mutex> lock(g_flow_mutex);
  for (const FlowPatch& p : g_flow_live)
    if (p.has_quit) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Item 2: the screen.
// ---------------------------------------------------------------------------------------------------------------
// The options manager (sub_82B14FC0, per frame; r3 = manager + 8, state at +16, the layout at manager + 4) runs the
// layout's page graph. Layout words: +120 pages (1064 bytes each), +40 page count, +132 / +140 a pending page request
// / the requested page, +136 the current target page. A list menu is a page found by the menu id (sub_82B43C18); a
// screen opened from a list is reached through the page's own exit link. The stock route to VISUAL PRESENTATION is
// page 24 (OPTIONS list) -> 41 (GAMEPLAY list) -> 72 (the screen): from the OPTIONS list the screen code 1300 only
// gets as far as page 41 (run 2092: it waits there for the click the GAMEPLAY list would have taken), so for the
// GRAPHICS entry we request page 72 ourselves, once (sub_82B43BD0, the call the page's own link makes).
constexpr uint32_t kPageGameplayList = 41, kPageVisualPresentation = 72, kPageOptionsList = 24;
inline bool g_advanced = false;
inline bool g_restart_message_pending = false;  // ACCEPT changed something that applies on the next start

// True while the VISUAL PRESENTATION screen code is open and the screen before it was the OPTIONS list.
inline bool InGraphicsFlow(uint8_t* base) {
  const uint32_t sm = W(base, kSmartMenuPtr);
  return sm && int32_t(W(base, sm + 364)) == int32_t(kScreenCodeVP) &&
         int32_t(W(base, sm + 356)) == int32_t(kOptionsMenu);
}

// The frames (objects of the screen's class) that belong to a GRAPHICS visit. Addresses are reused, so the set is
// emptied by the constructor / destructor hooks.
inline std::vector<uint32_t> g_gfx_frames;
inline bool IsGfx(uint32_t self) { return std::find(g_gfx_frames.begin(), g_gfx_frames.end(), self) != g_gfx_frames.end(); }
inline void Forget(uint32_t self) {
  g_gfx_frames.erase(std::remove(g_gfx_frames.begin(), g_gfx_frames.end(), self), g_gfx_frames.end());
}

// The frame's value / text tables (guest .rdata, 0x82062EB0..): per-row value counts, the text ids of a row's values
// (rows other than TARGETING INDICATORS) and of row 1's. Rows 0 and 2 use the first, row 1 the second, which overlap
// (0x82062ED4 is the third word of the first). Each is written for the length of one game call and put back.
constexpr uint32_t kCountsTable = 0x82062EB0;    // u32 x 7
constexpr uint32_t kValueTextTable = 0x82062ECC;  // u32 x 5 (value -> text id)
constexpr uint32_t kRow1TextTable = 0x82062ED4;   // u32 x 3 (row 1: value -> text id)

// The image's read-only data is mapped read-only on the host: the page holding the tables is made writable once.
inline bool EnsureTablesWritable() {
  static const bool ok = [] {
    DWORD old = 0;
    uint8_t* p = HostPtr(kCountsTable & ~0xFFFu);
    if (VirtualProtect(p, 0x1000, PAGE_READWRITE, &old)) return true;
    REXLOG_WARN("[wwe2k14][gfxmenu] could not make the value tables writable (error {}): GRAPHICS left out",
                GetLastError());
    return false;
  }();
  return ok;
}

struct TableGuard {
  uint32_t addr, n, saved[8] = {};
  TableGuard(uint8_t* base, uint32_t a, uint32_t words) : addr(a), n(words) {
    EnsureTablesWritable();
    for (uint32_t i = 0; i < n && i < 8; ++i) saved[i] = W(base, a + 4 * i);
  }
  ~TableGuard() {
    for (uint32_t i = 0; i < n && i < 8; ++i) Put32(addr + 4 * i, saved[i]);
  }
};

inline uint32_t RowValueTextBase(int row) {
  return row == 0 ? kTextVal0 : row == 1 ? kTextVal1 : row == 3 ? kTextVal3 : kTextVal2;
}

// Sprite names the screen's rows use (the layout's name table, run 2104: font000_Name%02d / font000_Item%02d the
// label and value texts, corsor_set%02d the arrows, gauge_set%02d the slider, beta_line1.. the row stripes).
inline std::map<std::string, uint32_t> g_name_ptrs;
inline uint32_t NamePtr(const std::string& s) {
  auto it = g_name_ptrs.find(s);
  if (it != g_name_ptrs.end()) return it->second;
  return g_name_ptrs[s] = AllocGuestString(s);
}
inline uint32_t g_ref_buf = 0;

// Hides the sprite called `name` in this frame's page (the sprite's visibility word is at +704).
inline void HideSprite(PPCContext& ctx, uint8_t* base, uint32_t self, const std::string& name) {
  if (!g_ref_buf) g_ref_buf = AllocGuest(16);
  if (!g_ref_buf) return;
  CallGuest(ctx, base, __imp__sub_82B07460, g_ref_buf, W(base, self + 4));
  const uint32_t sprite = CallGuest(ctx, base, __imp__sub_82B074F0, g_ref_buf, NamePtr(name));
  if (sprite) Put32(sprite + 704, 0);
}

inline void HideUnusedRows(PPCContext& ctx, uint8_t* base, uint32_t self) {
  char buf[40];
  for (int row = kRows; row < 7; ++row) {
    std::snprintf(buf, sizeof(buf), "font000_Name%02d", row), HideSprite(ctx, base, self, buf);
    std::snprintf(buf, sizeof(buf), "font000_Item%02d", row), HideSprite(ctx, base, self, buf);
    std::snprintf(buf, sizeof(buf), "corsor_set%02d", row), HideSprite(ctx, base, self, buf);
    std::snprintf(buf, sizeof(buf), "gauge_set%02d", row), HideSprite(ctx, base, self, buf);
    std::snprintf(buf, sizeof(buf), "beta_line%d", row + 1), HideSprite(ctx, base, self, buf);
  }
  HideSprite(ctx, base, self, "font001_L");
  HideSprite(ctx, base, self, "font001_R");
}

// The frame's seven 16-byte row records: {value, original, default, enabled}. Ours: rows 0..3 live, 4..6 disabled.
inline void ApplyRows(uint8_t* base, uint32_t self) {
  (void)base;
  for (int r = 0; r < kRows; ++r) {
    const uint32_t rec = self + 48 + 16 * r;
    Put32(rec + 0, uint32_t(g_vals[r]));
    Put32(rec + 4, uint32_t(g_vals[r]));
    Put32(rec + 8, 0);
    Put32(rec + 12, 1);
  }
  for (int r = kRows; r < 7; ++r) Put32(self + 48 + 16 * r + 12, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Item 3: QUIT TO DESKTOP (main menu list item 8, after SHOP).
//
// A on the item reaches the SmartMenu's "enter the highlighted item" (sub_82841648, r3 = the SmartMenu); for our item
// (menu 1, cursor 8) it is answered here and the game never sees it: the game's own YES / NO box asks "Quit to
// desktop?" (default NO). The message manager's state (+16: 5 idle, 1 opening, 2 waiting for an answer, 3 answered)
// and its selected button (+64: 0 YES, 1 NO; run 2124 / 2125) are read once a frame by the manager hook. After YES,
// and once the message manager is idle again (an autosave notice or any other box has finished), the quit is the
// one every scripted run ends with and Alt+F4 does: wwe2k14::g_request_quit (the window closes, the app shuts down in
// its normal order, exit code 0). Only the main menu's list has the item: nothing in a match can reach it.
// ---------------------------------------------------------------------------------------------------------------
inline bool g_quit_pending = false;    // the question is up
inline bool g_quit_seen_open = false;  // the box reached "waiting for an answer"
inline bool g_quit_go = false;         // YES: quit when the message manager is idle
inline uint32_t g_quit_sel = 1;        // the selected button while the box was open
inline uint64_t g_quit_frame = 0;      // frame of the last step

inline void QuitPoll(uint8_t* base) {
  const uint32_t mgr = W(base, kMessageManagerPtr);
  const uint32_t st = mgr ? W(base, mgr + 16) : 5;
  const uint64_t now = Frame();
  if (g_quit_pending) {
    // our box only: the post routine copies the event's type to +1800 (type 2 = YES / NO)
    if (st == 2 && W(base, mgr + 1800) == 2) {
      g_quit_seen_open = true;
      g_quit_sel = W(base, mgr + 64);
    } else if (g_quit_seen_open && (st == 3 || st == 5)) {
      g_quit_pending = false;
      g_quit_seen_open = false;
      REXLOG_INFO("[wwe2k14][gfxmenu] QUIT TO DESKTOP: answered {} frame={}", g_quit_sel == 0 ? "YES" : "NO", now);
      if (g_quit_sel == 0) g_quit_go = true, g_quit_frame = now;
    } else if (!g_quit_seen_open && now - g_quit_frame > 240) {
      g_quit_pending = false;
      REXLOG_WARN("[wwe2k14][gfxmenu] QUIT TO DESKTOP: the question never opened; cancelled");
    }
  }
  if (g_quit_go && st == 5 && now - g_quit_frame > 20) {
    g_quit_go = false;
    REXLOG_INFO("[wwe2k14][gfxmenu] QUIT TO DESKTOP: closing the game (frame {})", now);
    if (wwe2k14::g_request_quit) wwe2k14::g_request_quit();
  } else if (g_quit_go && now - g_quit_frame > 600) {
    g_quit_go = false;  // never fires from a stale answer
    REXLOG_WARN("[wwe2k14][gfxmenu] QUIT TO DESKTOP: the answer was YES but the message manager never went idle; dropped");
  }
}

}  // namespace wwe2k14::gfx_menu

// ---------------------------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------------------------

// Text lookup (the text manager's GetString): our ids are answered here; while the GRAPHICS screen is open the ids
// of the stock screen's header and row labels are swapped; everything else is the game's own.
REX_HOOK_RAW(sub_822563F8) {
  using namespace wwe2k14::gfx_menu;
  const uint32_t id = ctx.r4.u32, lr = uint32_t(ctx.lr);
  if (REXCVAR_GET(wwe2k14_gfx_menu) || REXCVAR_GET(wwe2k14_quit_item) || g_photo_pause_item) {
    uint32_t want = id;
    if (id >= kTextBase && id < kTextBase + 0x100) {
      want = id;
    } else if (id == 0xA04D || id == 0xA0A0 || id == 0xA100 || id == 0xA101 || id == 0x906 || id == 0xA102) {
      if (InGraphicsFlow(base)) {  // Session 33: 0xA102 (DISPLAY COUNTS, the stock row 3) -> TEXTURE FILTERING
        want = id == 0xA04D ? kTextGraphics : id == 0xA0A0 ? kTextPcSettings : id == 0xA100 ? kTextLabel[0]
               : id == 0xA101 ? kTextLabel[1] : id == 0xA102 ? kTextLabel[3] : kTextLabel[2];
      }
    }
    if (want != id || (id >= kTextBase && id < kTextBase + 0x100)) {
      if (const uint32_t s = TextFor(want)) {
        ctx.r3.u64 = s;
        if (wwe2k14::screen_text::Enabled()) wwe2k14::screen_text::Note(want, GuestStr(base, s, 120));
        return;
      }
    }
  }
  __imp__sub_822563F8(ctx, base);
  // Session 30: the screen text for scripts / live / bookmarks (src/screen_text.h; harness runs only).
  if (wwe2k14::screen_text::Enabled()) wwe2k14::screen_text::Note(id, GuestStr(base, ctx.r3.u32, 120));
  if (!REXCVAR_GET(wwe2k14_gfxmenu_probe) || !REXCVAR_GET(wwe2k14_gfxmenu_probe_text)) return;
  if (!REXCVAR_GET(wwe2k14_gfxmenu_probe_text_all)) {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    if (!g_text_seen.insert(id).second) return;
  }
  REXLOG_INFO("[wwe2k14][gfxmenu] text id {} (0x{:X}) = '{}' lr={:08X} frame={}", id, id,
              GuestStr(base, ctx.r3.u32), lr, Frame());
}

// The frame's constructor: a frame made for a GRAPHICS visit gets its rows set to ours at once.
REX_HOOK_RAW(sub_82B1B0C0) {
  using namespace wwe2k14::gfx_menu;
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82B1B0C0(ctx, base);
  Forget(self);
  if (REXCVAR_GET(wwe2k14_gfx_menu) && InGraphicsFlow(base)) {
    if (g_gfx_frames.empty()) LoadScreenValues();
    g_gfx_frames.push_back(self);
    ApplyRows(base, self);
  }
  if (!REXCVAR_GET(wwe2k14_gfxmenu_probe)) return;
  REXLOG_INFO("[wwe2k14][gfxmenu] VP ctor this={:08X} node={:08X} {} graphics={} frame={}", self, ctx.r4.u32,
              SmartMenuSummary(base), IsGfx(self), Frame());
  DumpObject(base, "VP", self, 168);
}

// The destructor (slot 0).
REX_HOOK_RAW(sub_82B1B138) {
  const uint32_t self = ctx.r3.u32;
  wwe2k14::gfx_menu::Forget(self);
  __imp__sub_82B1B138(ctx, base);
}

// Rows loaded from the settings (ctor with build 0, enter with build 1): ours instead of the game's.
REX_HOOK_RAW(sub_82B1AF78) {
  using namespace wwe2k14::gfx_menu;
  const uint32_t self = ctx.r3.u32, build = ctx.r4.u32;
  __imp__sub_82B1AF78(ctx, base);
  if (IsGfx(self)) {
    ApplyRows(base, self);
    if (build)
      for (uint32_t r = 0; r < uint32_t(kRows); ++r) CallGuest(ctx, base, sub_82B1ADF8, self, r, 2);
    HideUnusedRows(ctx, base, self);
  }
  if (!REXCVAR_GET(wwe2k14_gfxmenu_probe)) return;
  REXLOG_INFO("[wwe2k14][gfxmenu] VP load rows this={:08X} build={} graphics={} frame={}", self, int32_t(build),
              IsGfx(self), Frame());
  DumpObject(base, "VP", self, 168);
}

// Enter (slot 5).
REX_HOOK_RAW(sub_82B1B1A8) {
  using namespace wwe2k14::gfx_menu;
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82B1B1A8(ctx, base);
  if (IsGfx(self)) HideUnusedRows(ctx, base, self);
  if (!REXCVAR_GET(wwe2k14_gfxmenu_probe)) return;
  REXLOG_INFO("[wwe2k14][gfxmenu] VP enter this={:08X} {} graphics={} frame={}", self, SmartMenuSummary(base),
              IsGfx(self), Frame());
}

// Refresh one row (value text, arrows): the value tables hold our text ids for the length of the call.
REX_HOOK_RAW(sub_82B1ADF8) {
  using namespace wwe2k14::gfx_menu;
  const uint32_t self = ctx.r3.u32, row = ctx.r4.u32, mode = ctx.r5.u32;
  if (IsGfx(self)) {
    {
      if (row == 1) {
        TableGuard guard(base, kRow1TextTable, 3);
        for (uint32_t i = 0; i < 3; ++i) Put32(kRow1TextTable + 4 * i, kTextVal1 + i);
        __imp__sub_82B1ADF8(ctx, base);
      } else if (row == 0) {  // Session 33: up to 8 values (the words after the table are put back after the call)
        TableGuard guard(base, kValueTextTable, 8);
        for (int i = 0; i < g_up_count; ++i) Put32(kValueTextTable + 4 * i, kTextVal0 + uint32_t(g_up_order[i]));
        __imp__sub_82B1ADF8(ctx, base);
      } else {
        TableGuard guard(base, kValueTextTable, 5);
        const uint32_t tb = RowValueTextBase(int(row));
        for (uint32_t i = 0; i < 5; ++i) Put32(kValueTextTable + 4 * i, tb + i);
        __imp__sub_82B1ADF8(ctx, base);
      }
    }
    HideUnusedRows(ctx, base, self);
  } else {
    __imp__sub_82B1ADF8(ctx, base);
  }
  if (!REXCVAR_GET(wwe2k14_gfxmenu_probe)) return;
  REXLOG_INFO("[wwe2k14][gfxmenu] VP update row this={:08X} row={} mode={} graphics={} frame={}", self, int32_t(row),
              int32_t(mode), IsGfx(self), Frame());
}

// LEFT / RIGHT on a row (r4 = 0: right): the game's own handler, with our value counts in its table.
REX_HOOK_RAW(sub_82B1B460) {
  using namespace wwe2k14::gfx_menu;
  const uint32_t self = ctx.r3.u32, dir = ctx.r4.u32;
  const bool probe = REXCVAR_GET(wwe2k14_gfxmenu_probe);
  uint32_t before[7] = {};
  if (probe)
    for (uint32_t i = 0; i < 7; ++i) before[i] = W(base, self + 48 + 16 * i);
  if (IsGfx(self)) {
    TableGuard guard(base, kCountsTable, 7);
    for (int r = 0; r < kRows; ++r) Put32(kCountsTable + 4 * r, uint32_t(g_counts[r]));
    __imp__sub_82B1B460(ctx, base);
    for (int r = 0; r < kRows; ++r) g_vals[r] = int(W(base, self + 48 + 16 * r));
  } else {
    __imp__sub_82B1B460(ctx, base);
  }
  if (!probe) return;
  REXLOG_INFO("[wwe2k14][gfxmenu] VP change this={:08X} dir={} values {},{},{},{},{},{},{} -> {},{},{},{},{},{},{} "
              "graphics={} frame={}",
              self, int32_t(dir), before[0], before[1], before[2], before[3], before[4], before[5], before[6],
              W(base, self + 48), W(base, self + 64), W(base, self + 80), W(base, self + 96), W(base, self + 112),
              W(base, self + 128), W(base, self + 144), IsGfx(self), Frame());
}

// ACCEPT (slot 12): for our screen the settings file, not the game's save.
REX_HOOK_RAW(sub_82B1B2B8) {
  using namespace wwe2k14::gfx_menu;
  const uint32_t self = ctx.r3.u32;
  if (IsGfx(self)) {
    for (int r = 0; r < kRows; ++r) g_vals[r] = int(W(base, self + 48 + 16 * r));
    const bool restart = CommitScreenValues();
    for (int r = 0; r < kRows; ++r) Put32(self + 52 + 16 * r, uint32_t(g_vals[r]));  // saved: nothing left to confirm
    if (restart) g_restart_message_pending = true;  // shown when the screen has closed (manager hook)
    ctx.r3.u64 = 1;
    if (REXCVAR_GET(wwe2k14_gfxmenu_probe))
      REXLOG_INFO("[wwe2k14][gfxmenu] VP commit (graphics) this={:08X} restart={} frame={}", self, restart, Frame());
    return;
  }
  __imp__sub_82B1B2B8(ctx, base);
  if (!REXCVAR_GET(wwe2k14_gfxmenu_probe)) return;
  REXLOG_INFO("[wwe2k14][gfxmenu] VP commit this={:08X} -> {} frame={}", self, int32_t(ctx.r3.u32), Frame());
}

// Leave (slot 13): the game's own check and "keep these settings?" question.
REX_HOOK_RAW(sub_82B1B378) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82B1B378(ctx, base);
  if (!REXCVAR_GET(wwe2k14_gfxmenu_probe)) return;
  using namespace wwe2k14::gfx_menu;
  REXLOG_INFO("[wwe2k14][gfxmenu] VP can-exit this={:08X} -> {} flag44={} frame={}", self, int32_t(ctx.r3.u32),
              int32_t(W(base, self + 44)), Frame());
}

// Flow table lookup (menu id, item index -> record): the first call with a table that matches the OPTIONS list
// builds the longer table (see PatchFlowTable); every later call is one comparison.
REX_HOOK_RAW(sub_83424808) {
  if (REXCVAR_GET(wwe2k14_gfx_menu) || REXCVAR_GET(wwe2k14_quit_item))
    wwe2k14::gfx_menu::EnsureFlowPatched(base, ctx.r3.u32);
  __imp__sub_83424808(ctx, base);
}

// A flow table's destructor (r3 = the table's data object, T = r3 + 4): the game frees [T+4] and [T+88] with its own
// allocator, so a table we patched gets the game's own arrays back first (see FlowPatch). Reached from the holder's
// destructor and from sub_83425FC8 whenever a match, Create-a-Wrestler or the story takes the menus down.
REX_HOOK_RAW(sub_83425C68) {
  wwe2k14::gfx_menu::ReleaseFlowPatch(base, ctx.r3.u32 + 4);
  __imp__sub_83425C68(ctx, base);
}

// The options manager's per-frame state machine (vtable method, r3 = manager + 8).
REX_HOOK_RAW(sub_82B14FC0) {
  const uint32_t self = ctx.r3.u32;
  using namespace wwe2k14::gfx_menu;
  const bool probe = REXCVAR_GET(wwe2k14_gfxmenu_probe);
  const bool on = REXCVAR_GET(wwe2k14_gfx_menu);
  if (REXCVAR_GET(wwe2k14_quit_item)) QuitPoll(base);
  static uint64_t last_key = ~0ull;
  uint32_t st = 0, lay = 0, p88 = 0, p136 = 0, menu = 0, ctl = 0;
  if (probe || on) {
    const uint32_t m = self - 8;
    st = W(base, self + 16);
    lay = W(base, m + 4);
    p88 = lay ? W(base, lay + 88) : 0xFFFFFFFFu;
    p136 = lay ? W(base, lay + 136) : 0xFFFFFFFFu;
    const uint32_t sm = W(base, kSmartMenuPtr);
    menu = sm ? W(base, sm + 364) : 0xFFFFFFFFu;
    ctl = sm ? W(base, W(base, sm + 11412) + 32) : 0xFFFFFFFFu;
    if (on) {
      if (g_restart_message_pending && menu == kOptionsMenu && st == 1) {
        // back on the OPTIONS list: say it once (when another box is up it is tried again on the next frame)
        const bool ok = PostMessageBox(ctx, base, uint32_t(REXCVAR_GET(wwe2k14_gfxmenu_msg_type)), kTextRestart,
                                     uint32_t(REXCVAR_GET(wwe2k14_gfxmenu_msg_arg)));
        if (ok) {
          g_restart_message_pending = false;
          REXLOG_INFO("[wwe2k14][gfxmenu] restart message posted (type {}) frame={}",
                      REXCVAR_GET(wwe2k14_gfxmenu_msg_type), Frame());
        }
      }
      if (menu != kScreenCodeVP) {
        g_advanced = false;
      } else if (!g_advanced && st == 2 && lay && p136 != kPageOptionsList && p136 != kPageVisualPresentation &&
                 int32_t(p136) >= 0 && W(base, lay + 132) == 0 && InGraphicsFlow(base)) {
        // the layout's own link from the OPTIONS list page has landed on a page of its choosing (41 on the first
        // visit, 63 = SAVE MANAGER's on the next: it follows its remembered selection); take the screen's page
        const uint32_t pages = W(base, lay + 120), count = W(base, lay + 40);
        if (pages && count > kPageVisualPresentation) {
          g_advanced = true;
          // request page 72 (the screen) as the click on a GAMEPLAY list item would: (layout, page, element, 1,
          // return page, return item), sub_82B43BD0; the return page is the OPTIONS list (24), item GRAPHICS (3)
          CallGuest(ctx, base, __imp__sub_82B43BD0, lay, kPageVisualPresentation, 0, 1, kPageOptionsList, 3);
          if (probe)
            REXLOG_INFO("[wwe2k14][gfxmenu] GRAPHICS: requested page {} (the layout had gone to page {}; layout {:08X}) "
                        "frame={}",
                        kPageVisualPresentation, int32_t(p136), lay, Frame());
        }
      }
    }
  }
  if (REXCVAR_GET(wwe2k14_gfxmenu_probe_mgr)) {
    static std::vector<uint32_t> prev;
    const uint32_t mgr = W(base, kMessageManagerPtr);
    if (mgr) {
      const uint32_t n = 520;  // words (2080 bytes)
      if (prev.size() != n) prev.assign(n, 0xDEADBEEFu);
      std::string changes;
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v = W(base, mgr + 4 * i);
        if (v != prev[i]) {
          if (prev[i] != 0xDEADBEEFu && changes.size() < 600)
            changes += fmt::format(" +{}:{:X}->{:X}", 4 * i, prev[i], v);
          prev[i] = v;
        }
      }
      if (!changes.empty()) REXLOG_INFO("[wwe2k14][gfxmenu] message manager {:08X} changed:{} frame={}", mgr, changes, Frame());
    }
  }
  __imp__sub_82B14FC0(ctx, base);
  if (!probe) return;
  const uint32_t st2 = W(base, self + 16);
  const uint64_t key = (uint64_t(st2) << 48) ^ (uint64_t(p88 & 0xFFFF) << 32) ^ (uint64_t(p136 & 0xFFFF) << 16) ^
                       uint64_t(menu & 0xFFFF) ^ (uint64_t(ctl & 0xFF) << 40);
  if (key == last_key) return;
  last_key = key;
  REXLOG_INFO("[wwe2k14][gfxmenu] manager state {}->{} layout={:08X} page={} target_page={} menu={} ctrl_state={} frame={}",
              st, st2, lay, int32_t(p88), int32_t(p136), int32_t(menu), int32_t(ctl), Frame());
}

// The SmartMenu's "enter the highlighted item" (A on a list item): QUIT TO DESKTOP is ours (item 3).
REX_HOOK_RAW(sub_82841648) {
  using namespace wwe2k14::gfx_menu;
  // Only while our patch of the main menu's list is live: a list that was left stock (another game version, a list
  // that did not match) has a real item 8, which the game answers itself.
  if (REXCVAR_GET(wwe2k14_quit_item) && QuitPatchLive()) {
    const uint32_t sm = ctx.r3.u32;
    if (sm && int32_t(W(base, sm + 364)) == int32_t(kMainMenu) && W(base, sm + 352) == kQuitItem) {
      if (!g_quit_pending && !g_quit_go) {
        const bool ok = PostMessageBox(ctx, base, 2, kTextQuitAsk);  // type 2: YES / NO, default NO
        g_quit_pending = ok, g_quit_seen_open = false, g_quit_sel = 1, g_quit_frame = Frame();
        REXLOG_INFO("[wwe2k14][gfxmenu] QUIT TO DESKTOP: question {} frame={}",
                    ok ? "posted" : "NOT posted (another box is up: press A again)", Frame());
      }
      return;
    }
  }
  __imp__sub_82841648(ctx, base);
}
