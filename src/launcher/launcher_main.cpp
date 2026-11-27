// wwe2k14 launcher (Session 17 item 1C; Session 25: the launcher for players; Session 31: the game's menu look,
// launcher_theme.h / game_font.h): a separate Windows program next to wwe2k14.exe. The page list (PLAY, SETTINGS,
// MODS, ABOUT) sits on a red parallelogram like the game's main menu:
//
//   Play:      the logo, PLAY GAME (boots to the menus with the player's settings) and QUICK MATCH (its own screen: the
//              game's PLAY menu as columns, or a saved match setup), and Advanced (presets Replay, Re-shoot, Seed,
//              Director; the exact command line).
//   Mods:      Trons (assign a .mp4 / .mov / .mkv / .bik2 per character, copied into the mods folder), Titles (the
//              title reveals, transparent), Logos (PNGs as real Create-a-Logo logos through the game: logo_save.h).
//   Settings:  game data (checked against the supported disc), resolution, frame rate, upscaling, reports, Advanced.
//   About:     the project, links, credits, the licences file.
//   Footer:    Report a problem (a .zip of the latest log, the performance report, settings, version, PC basics).
// A first-run wizard; an abnormal game exit offers Try again / Report a problem.
//
// Dear ImGui on Win32 + Direct3D 11 (both part of Windows) and the static C runtime: nothing extra to install.
//
// Command-line automation (used for unattended checks; each does what the matching button does):
//   --page play|mods|settings    --mods-tab trons|titles|logos|music    --advanced (open the Advanced sections)
//   --game-data DIR  --user-data DIR (this run only; implies --no-save)    --wizard | --no-wizard   --rtx (show DLSS)
//   --fake-igpu (Session 37: the choices for integrated graphics: light renderer, 720p, no FSR)
//   --fake-no-interlock (Session 54: a card without fragment shader interlock)
//   --logo-add PNG  --logo-import (back up, run the game's menus, check)   --wait-game (until the game has closed)
//   --backup  --restore N (0 = newest backup)  --report  --no-open (don't open folders)
//   --settings FILE (another settings file: a fresh profile)   --play  --quick-match  --capture NAME
//   --quick-type N (Quick Match: build match type N)   --page about|quick|advanced (Session 31)
//   --page wm30 (Play -> 30 Years of WrestleMania)   --wm30-era N (its era tab)   --wm30-play N (start match N; Session 34)
//   --settings-section N (Settings: 0 game, 1 display, 2 graphics, 3 advanced; Session 31)
//   --show-crash CODE | --show-report (open that popup, for a look; Session 31)   --titles-view list|grid
//   --show-keys (Session 67: the KEYBOARD CONTROLS window)   --keys-capture N (its "press a key" for control N, the
//                           keyboard_binds.h kBinds order)   --keys-hot N (light control N, as a hovered row does)
//   --update-check (check for updates even in a +dev build)   --update-file JSON (a release from a file, not GitHub)
//   --update-version VER (pretend this version)   --show-update (open the update details, if one was found)
//   --update-install / --update-finish / --update-cancel (Session 81: press UPDATE NOW / RESTART AND FINISH / Cancel;
//                           --update-file also lets the install take file:///PATH downloads, for fake releases)
//   --kill-game CODE (end the game with this exit code: a test of the crash dialog)
//   --tab trons|titles|play --select TRON          --assign TRON FILE     --revert TRON
//   --assign-alpha TRON FILE (stacked alpha)       --banner TRON original|video|none|FILE   --title ID
//   --wait N                (N idle steps, 4 frames each: let previews load before a --shot)
//   --scenario NAME         --mods 0|1             --preset play|replay|reshoot|seed|director
//   --harness ROUTE         (run through tools\bench\run_list.ps1: frozen save + run lock)
//   --launch                --shot FILE.bmp (window screenshot after the UI settles)
//   --exit                  (quit after the steps)    --log FILE (what the steps did)
//   --no-save               (don't write the settings file)
//   --perf-test             (Session 39: run the performance test, test builds with perf_test.txt only (Session 41);
//                           waits for it)
//   --show-perf-test        (open its window, for a look)
//   --xdet-check            (Session 77: run the determinism check, ABOUT; waits for it and its zip)
//   --show-xdet-check       (open its window, for a look)
//   --export-for-phone ZIP  (Session 98: Trons -> Export for phone... into ZIP, no Save dialog; waits for it)

#include "../thread_name.h"  // S102 H1
#include "launcher_core.h"
#include "../tron_slots_ui_core.h"  // S110 TRON-B: the custom-tron screens' decisions
#include "tab_strip.h"
#include "bench_button.h"  // S102 U4: the benchmark button's numbers and results writer
#include "stars_start.h"   // S110 HELPERSTART: the superstars pack before every start that loads the save

#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11Multithread
#include <shellapi.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <condition_variable>
#include <random>
#include <mutex>
#include <thread>
#include <cstdarg>
#include <deque>
#include <memory>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"  // TableSetColumnSortDirection (sync the header arrows with the saved sort)
#include "misc/cpp/imgui_stdlib.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "game_font.h"
#include "keyboard_binds.h"  // Session 67: KEYBOARD CONTROLS (the keys of each pad control, keybinds.toml)
#include "launcher_system.h"
#include "update_check.h"
#include "markdown_imgui.h"
#include "update_install.h"
#include "launcher_theme.h"
#include "logo_save.h"
#include "dlc_names.h"  // Session 32: DLC wrestlers' names from the player's DLC files (needs stb_image above)
#include "wm30_list.h"  // Session 34: the 30 Years of WrestleMania matches from the player's game files
#include "../diag_plan.h"  // Session 95: the diagnostic run (shared with the phone launcher)
#include "../diag_logcut.h"  // Session 95 (diag4): the game log cut for the zip (shared)
#include "report_redact.h"  // S108 MOB: Redact, shared with the phone launcher
#include "cpu_report.h"  // S108 MOB: the CPU feature lines of about.txt
#include "report_findings.h"  // S105 C0b: findings.txt in the report zip (the C++ port of tools\report\triage.py; shared with the phone launcher)
#include "../gfx_applied.h"  // S102 S0-1: SuggestIntegrated (shared with the Surface bench's profile generator)
#include "../vk_devices.h"  // Session 37: integrated graphics? interlock? (the renderer and FSR choices)
#include "../tron_zip.h"  // Session 98: EXPORT FOR PHONE (the tron previews as one zip for the phone launcher)
#include "../audio_import/audio_import.h"  // lane 283: song import (decode, loudness-normalise, cover art)
#include "../importer/importer_core.h"  // Session 98: the SUPERSTARS tab's table (LoadPack / SavePack)
#include "../importer/screen_text.h"  // S110 SEAT-C: the count line, "not seated", the old pack's conversion line

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using namespace launcher;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

// GPU objects released after the frame is presented: something drawn earlier in the frame may still use them (a crash
// in the driver when a preview switched mid-frame and released the texture a tile had already queued).
std::vector<IUnknown*> g_release_after_present;
void ReleaseLater(IUnknown* p) {
  if (p) g_release_after_present.push_back(p);
}
void FlushReleases() {
  for (IUnknown* p : g_release_after_present) p->Release();
  g_release_after_present.clear();
}

// Media Foundation's link to our Direct3D 11 device, so video decodes on the GPU (the decoder and the colour
// conversion run on the video engine; frames stay in GPU memory). Null if the device can't do video.
IMFDXGIDeviceManager* g_dxgi_manager = nullptr;
HWND g_hwnd = nullptr;
// Session 31: the menu font (Windows' Arial Black: heavy, wide capitals like the game's menus; nothing to ship).
// Null = the regular font at the asked size.
ImFont* g_font_menu = nullptr;
// --debug-preview FILE: log every shown preview frame (time stamp, path, centre pixel) for diagnosis.
FILE* g_preview_log = nullptr;
void LogPreviewPixel(ID3D11Texture2D* tex, LONGLONG ts, bool gpu, int w, int h) {
  if (!g_preview_log || !tex) return;
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  d.Width = d.Height = 1, d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(g_device->CreateTexture2D(&d, nullptr, &st))) return;
  const D3D11_BOX box{UINT(w / 2), UINT(h / 3), 0, UINT(w / 2 + 1), UINT(h / 3 + 1), 1};
  g_context->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, &box);
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(g_context->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
    const uint8_t* px = static_cast<const uint8_t*>(m.pData);
    std::fprintf(g_preview_log, "ts %.3f %s fmt %d bgra %d %d %d %d\n", ts / 1e7, gpu ? "gpu" : "sw", int(d.Format),
                 px[0], px[1], px[2], px[3]);
    std::fflush(g_preview_log);
    g_context->Unmap(st, 0);
  }
  st->Release();
}
void WakeUi() {  // a background thread has something for the UI (it may be idle-waiting)
  if (g_hwnd) PostMessageW(g_hwnd, WM_APP, 0, 0);
}

UINT g_resize_w = 0, g_resize_h = 0;

void CreateRenderTarget() {
  ID3D11Texture2D* back = nullptr;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
  back->Release();
}
void CleanupRenderTarget() {
  if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
}

bool CreateDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate = {60, 1};
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                             D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
  if (FAILED(hr))  // no video support: the device without it (previews then decode in software)
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                       &sd, &g_swap, &g_device, &got, &g_context);
  if (hr == DXGI_ERROR_UNSUPPORTED)
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                       &g_swap, &g_device, &got, &g_context);
  if (FAILED(hr)) return false;
  CreateRenderTarget();
  // GPU video decoding: Media Foundation uses this device from its own threads, so turn on its locking, and hand it
  // to Media Foundation through a DXGI device manager.
  ID3D11Multithread* mt = nullptr;
  if (SUCCEEDED(g_context->QueryInterface(IID_PPV_ARGS(&mt)))) mt->SetMultithreadProtected(TRUE), mt->Release();
  UINT token = 0;
  MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &g_dxgi_manager)) &&
      FAILED(g_dxgi_manager->ResetDevice(g_device, token)))
    g_dxgi_manager->Release(), g_dxgi_manager = nullptr;
  return true;
}

// Session 67: KEYBOARD CONTROLS' "press a key". While it waits, the window takes key presses before ImGui (so the
// key does not also move ImGui's focus): the scan code (the key's position, what the game reads) and the modifiers.
struct KeyCapture : keys::CaptureState {  // (the state machine is keyboard_binds.h: Shift alone, Shift+Up, ...)
  bool active = false;
};
KeyCapture g_key_capture;

// Lane 283: set while the MUSIC converter works; the window's close button then asks first (WM_CLOSE below).
std::atomic<bool> g_music_busy{false}, g_close_asked{false};

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (g_key_capture.active) {
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(wp == VK_F4 && GetKeyState(VK_MENU) < 0)) {  // Alt+F4 closes
      keys::CaptureKeyDown(g_key_capture, wp, lp, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0,
                           GetKeyState(VK_MENU) < 0, uint32_t(GetMessageTime()));
      return 0;
    }
    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE))
      keys::CaptureFocusLost(g_key_capture);  // (Alt+Tab: the Alt must not bind, its release goes to another window)
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) keys::CaptureKeyUp(g_key_capture, wp);  // (a modifier let go alone)
    if (msg == WM_CHAR || msg == WM_SYSCHAR) return 0;  // (key ups go on to ImGui: it saw no down, nothing sticks)
  }
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) g_resize_w = LOWORD(lp), g_resize_h = HIWORD(lp);
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_CLOSE:
      if (g_music_busy.load()) {  // a song is converting: the window asks (wait / cancel and close) instead of dropping it
        g_close_asked = true;
        return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

#include "app/win_helpers.inc"  // split: win_helpers
#include "app/media_player.inc"  // split: media_player

class App {
 public:
  explicit App(HWND hwnd) : hwnd_(hwnd) {}

  Settings s;
  fs::path settings_path;
  bool save_settings = true;
  std::deque<Step> steps;
  fs::path log_path;
  bool exit_after_steps = false;

  #include "app/load_update.inc"  // split: load_update
  #include "app/automation_frame.inc"  // split: automation_frame
  #include "app/widgets_nav.inc"  // split: widgets_nav
  #include "app/play.inc"  // split: play
  #include "app/game_start.inc"  // split: game_start
  #include "app/report.inc"  // split: report
  #include "app/perf_bench_check.inc"  // split: perf_bench_check
  #include "app/diag.inc"  // split: diag
  #include "app/mods_superstars.inc"  // split: mods_superstars
  #include "app/music.inc"  // split: music
  #include "app/logos.inc"  // split: logos
  #include "app/settings_data_dlc.inc"  // split: settings_data_dlc
  #include "app/settings_pages.inc"  // split: settings_pages
  #include "app/keys.inc"  // split: keys
  #include "app/wizard.inc"  // split: wizard
  #include "app/trons_media.inc"  // split: trons_media
  #include "app/trons_ui.inc"  // split: trons_ui
  #include "app/titles.inc"  // split: titles
  #include "app/advanced_play.inc"  // split: advanced_play
};

void ApplyStyle(float dpi) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  // Session 31: sharper corners and see-through panels over the dotted background (the game's menus).
  st.WindowRounding = 0, st.FrameRounding = 2, st.GrabRounding = 2, st.TabRounding = 0, st.ChildRounding = 0;
  st.PopupRounding = 2, st.FramePadding = ImVec2(9, 6), st.ItemSpacing = ImVec2(9, 7), st.WindowPadding = ImVec2(16, 14);
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.08f, 0.095f, 1);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.075f, 0.088f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.135f, 0.16f, 0.95f);
  c[ImGuiCol_Header] = ImVec4(0.55f, 0.12f, 0.12f, 0.55f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.65f, 0.16f, 0.16f, 0.75f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.72f, 0.18f, 0.18f, 1);
  c[ImGuiCol_Tab] = ImVec4(0.16f, 0.17f, 0.2f, 1);
  c[ImGuiCol_TabHovered] = ImVec4(0.65f, 0.16f, 0.16f, 1);
  c[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.12f, 0.12f, 1);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.205f, 0.24f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.11f, 0.13f, 1);  // Session 31: the game's red on hover
  c[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.07f, 0.09f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.35f, 0.3f, 1);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
  st.ScaleAllSizes(dpi);
  st.FontScaleDpi = dpi;
}

std::vector<Step> ParseArgs(App& app) {
  std::vector<Step> out;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) {
    const std::string a = Narrow(argv[i]);
    auto next = [&]() { return i + 1 < argc ? Narrow(argv[++i]) : std::string(); };
    if (a == "--tab") out.push_back({"tab", next()});
    else if (a == "--select") out.push_back({"select", next()});
    else if (a == "--assign") { std::string t = next(); out.push_back({"assign", t, next()}); }
    else if (a == "--revert") out.push_back({"revert", next()});
    else if (a == "--assign-alpha") { std::string t = next(); out.push_back({"assign_alpha", t, next()}); }
    else if (a == "--banner") { std::string t = next(); out.push_back({"banner", t, next()}); }
    else if (a == "--title") out.push_back({"title", next()});
    else if (a == "--wait") for (int n = std::max(0, std::atoi(next().c_str())); n > 0; --n) out.push_back({"wait"});
    else if (a == "--scenario") out.push_back({"scenario", next()});
    else if (a == "--mods") out.push_back({"mods", next()});
    else if (a == "--preset") out.push_back({"preset", next()});
    else if (a == "--harness") out.push_back({"harness", next()});
    else if (a == "--launch") out.push_back({"launch"});
    else if (a == "--perf-test") out.push_back({"perf_test"}), out.push_back({"wait_perf_test"});  // Session 39
    else if (a == "--xdet-check") out.push_back({"xdet_check"}), out.push_back({"wait_xdet_check"});  // Session 77
    else if (a == "--show-xdet-check") out.push_back({"show_xdet_check"});
    else if (a == "--diag-run") out.push_back({"diag_run"}), out.push_back({"wait_diag_run"});  // Session 95
    else if (a == "--show-perf-test") out.push_back({"show_perf_test"});
    else if (a == "--shot") out.push_back({"shot", next()});
    else if (a == "--exit") app.exit_after_steps = true;
    else if (a == "--log") app.log_path = fs::path(Widen(next()));
    else if (a == "--no-save") app.save_settings = false;
    else if (a == "--view") out.push_back({"view", next()});
    else if (a == "--sort") out.push_back({"sort", next()});
    else if (a == "--reset-play") out.push_back({"reset_play"});
    else if (a == "--hover") out.push_back({"hover", next()});
    else if (a == "--debug-preview") g_preview_log = _wfopen(Widen(next()).c_str(), L"w");
    // Session 25
    else if (a == "--page") out.push_back({"page", next()});
    else if (a == "--mods-tab") out.push_back({"mods_tab", next()});
    else if (a == "--advanced") app.OpenAdvanced();
    else if (a == "--game-data") app.override_game_data = next(), app.save_settings = false;
    else if (a == "--user-data") app.override_user_data = next(), app.save_settings = false;
    else if (a == "--wizard") app.wizard_mode_ = 1;
    else if (a == "--no-wizard") app.wizard_mode_ = -1;
    else if (a == "--rtx") app.ForceRtx();
    else if (a == "--fake-igpu") app.FakeIntegratedGpu();  // Session 37
    else if (a == "--fake-no-interlock") app.FakeNoInterlock();  // Session 54
    else if (a == "--logo-add") out.push_back({"logo_add", next()});
    else if (a == "--logo-import") out.push_back({"logo_import"});
    else if (a == "--logo-size") out.push_back({"logo_size", next()});  // Session 53: 128 or 256
    else if (a == "--wait-game") out.push_back({"wait_game"});
    else if (a == "--wait-stars") out.push_back({"wait_stars"});  // Session 101 (lane 199)
    else if (a == "--backup") out.push_back({"backup"});
    else if (a == "--restore") out.push_back({"restore", next()});
    else if (a == "--report") out.push_back({"report"});
    else if (a == "--no-open") app.NoOpen();
    else if (a == "--settings") app.settings_path = fs::path(Widen(next()));  // another settings file (a fresh profile)
    else if (a == "--play") out.push_back({"play"});
    else if (a == "--start-game") app.start_game_ = true;  // Session 53: the game at once, the window hidden
    else if (a == "--shortcut-dir") app.shortcut_dir_ = fs::path(Widen(next()));  // Session 53 tests
    else if (a == "--create-shortcut") out.push_back({"create_shortcut"});
    else if (a == "--build-previews") out.push_back({"build_previews"});  // Session 53 item 2.2 test
    else if (a == "--export-for-phone") out.push_back({"export_phone", next()}), out.push_back({"wait_export_phone"});  // Session 98
    else if (a == "--local-data") g_local_data_override = fs::path(Widen(next()));
    else if (a == "--quick-match") out.push_back({"quick_match"});
    else if (a == "--quick-type") out.push_back({"quick_type", next()});
    else if (a == "--wm30-era") out.push_back({"wm30_era", next()});  // Session 34
    else if (a == "--wm30-play") out.push_back({"wm30_play", next()});  // Session 34
    else if (a == "--settings-section") out.push_back({"settings_section", next()});
    else if (a == "--show-crash") out.push_back({"show_crash", next()});  // the crash popup with this code (a look)
    else if (a == "--show-report") out.push_back({"show_report"});
    else if (a == "--show-keys") out.push_back({"show_keys"});  // Session 67
    else if (a == "--keys-capture") out.push_back({"keys_capture", next()});
    else if (a == "--keys-hot") out.push_back({"keys_hot", next()});
    else if (a == "--titles-view") out.push_back({"titles_view", next()});
    // Session 31: update check tests.
    else if (a == "--update-check") app.update_force_ = true;          // check even in a +dev build
    else if (a == "--update-file") app.update_file_ = next(), app.update_force_ = true, app.update_test_files_ = true;  // GitHub's answer from a file
    else if (a == "--update-version") app.update_version_ = next();   // pretend this version
    else if (a == "--show-update") out.push_back({"show_update"});
    else if (a == "--update-install") out.push_back({"update_install"});  // Session 81 tests: UPDATE NOW, then
    else if (a == "--update-finish") out.push_back({"update_finish"});    // RESTART AND FINISH (when it is ready)
    else if (a == "--update-cancel") out.push_back({"update_cancel"});
    else if (a == "--capture") out.push_back({"capture", next()});
    else if (a == "--kill-game") out.push_back({"kill_game", next()});
    else if (a == "--wizard-step") out.push_back({"wizard_step", next()});
    else if (a == "--no-rtx") out.push_back({"no_rtx"});  // show the Settings page as on a non-RTX card
  }
  LocalFree(argv);
  return out;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  {  // Session 81: the update helper (a copy of this exe in %TEMP%, update_install.h): no window, no Direct3D
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && (std::wstring(argv[1]) == L"--apply-update" || std::wstring(argv[1]) == L"--restore-update"))
      return upd::RunHelper(std::vector<std::wstring>(argv + 1, argv + argc));
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ImGui_ImplWin32_EnableDpiAwareness();
  const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

  // Icon resource 1 (game_logo.png, embedded by the build when present), else the default application icon.
  HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, icon, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr,
                 L"wwe2k14_launcher", icon};
  RegisterClassExW(&wc);
  Settings pre = LoadSettings(SettingsPath());
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"WWE 2K14 Launcher", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            std::max(900, int(pre.window_w)), std::max(600, int(pre.window_h)), nullptr, nullptr,
                            inst, nullptr);
  g_hwnd = hwnd;
  if (!CreateDevice(hwnd)) {
    MessageBoxW(hwnd, L"Direct3D 11 is not available.", L"WWE 2K14 Launcher", MB_ICONERROR);
    return 1;
  }
  // Session 53: --start-game keeps the window hidden (the App shows it if the game can't start).
  bool start_game = false;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) start_game |= std::wstring(argv[i]) == L"--start-game";
    if (argv) LocalFree(argv);
  }
  if (!start_game) ShowWindow(hwnd, SW_SHOWDEFAULT), UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ApplyStyle(dpi);
  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  const fs::path font = fs::path(windir) / "Fonts" / "segoeui.ttf";
  std::error_code ec;
  if (fs::exists(font, ec)) io.Fonts->AddFontFromFileTTF(PathStr(font).c_str(), 17.0f);
  // Session 31: the fallback menu face when the game's own (game_font.h) can't be read.
  const fs::path menu_font = fs::path(windir) / "Fonts" / "ariblk.ttf";
  if (fs::exists(menu_font, ec)) g_font_menu = io.Fonts->AddFontFromFileTTF(PathStr(menu_font).c_str(), 17.0f);
  ImGui::GetStyle().FontSizeBase = 17.0f;
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);

  App app(hwnd);
  std::vector<Step> steps = ParseArgs(app);
  app.SetAutomation(!steps.empty());  // lane 283: launcher steps (--harness ROUTE, --launch --wait-game ...) never wait for or run a song conversion
  if (!app.log_path.empty()) std::ofstream(app.log_path, std::ios::trunc) << "launcher start\n";
  app.Load();
  for (auto& st : steps) app.steps.push_back(st);
  app.MaybeWizard(!steps.empty());
  app.BeginDirectStart(!steps.empty());  // Session 53: --start-game / QUICK START

  int frame = 0;
  bool done = false;
  int quiet = 0;  // frames drawn since the last message (input, resize, a background thread's wake-up)
  while (!done) {
    // Idle: nothing playing or loading, no automation, and the UI has settled after the last input -> wait for the
    // next message (or 0.5 s, for the status line and the 2-second file checks) instead of drawing at 60 fps.
    if (quiet > 8 && !app.Animating() && app.steps.empty() && !app.exit_after_steps)
      MsgWaitForMultipleObjectsEx(0, nullptr, 500, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG msg;
    bool any = false;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      any = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    quiet = any ? 0 : quiet + 1;
    if (done) break;
    if (g_resize_w) {
      CleanupRenderTarget();
      g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
      g_resize_w = g_resize_h = 0;
      CreateRenderTarget();
    }
    // Automation: one step every few frames (shots wait for the UI to settle after the previous step).
    ++frame;
    bool take_shot = false;
    std::string shot_path;
    if (frame > 6 && frame % 4 == 0 && !app.steps.empty()) {
      Step st = app.steps.front();
      app.steps.pop_front();
      if (st.op == "shot") take_shot = true, shot_path = st.a;
      else if (!app.RunStep(st)) app.steps.push_front(st);  // still waiting (--wait-game)
      else app.Log("step " + st.op + " " + st.a + " " + st.b);
    } else if (app.exit_after_steps && app.steps.empty() && frame > 10) {
      done = true;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.Frame();
    ImGui::Render();
    const float clear[4] = {0.075f, 0.08f, 0.095f, 1};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (take_shot) app.RunStep({"shot", shot_path});
    g_swap->Present(1, 0);
    FlushReleases();
  }

  app.Shutdown();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  CleanupRenderTarget();
  if (g_swap) g_swap->Release();
  if (g_context) g_context->Release();
  if (g_device) g_device->Release();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, inst);
  return 0;
}
