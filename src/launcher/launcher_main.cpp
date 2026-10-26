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
//   --page play|mods|settings    --mods-tab trons|titles|logos    --advanced (open the Advanced sections)
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

#include "launcher_core.h"

#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11Multithread
#include <shellapi.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
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
#include "launcher_theme.h"
#include "logo_save.h"
#include "dlc_names.h"  // Session 32: DLC wrestlers' names from the player's DLC files (needs stb_image above)
#include "wm30_list.h"  // Session 34: the 30 Years of WrestleMania matches from the player's game files
#include "../vk_devices.h"  // Session 37: integrated graphics? interlock? (the renderer and FSR choices)

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
struct KeyCapture {
  bool active = false, got = false;
  WPARAM vk = 0;
  LPARAM lp = 0;
  bool shift = false, ctrl = false, alt = false;
};
KeyCapture g_key_capture;

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (g_key_capture.active) {
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(wp == VK_F4 && GetKeyState(VK_MENU) < 0)) {  // Alt+F4 closes
      if (!g_key_capture.got && !keys::IsModifierKey(wp)) {
        g_key_capture.got = true, g_key_capture.vk = wp, g_key_capture.lp = lp;
        g_key_capture.shift = GetKeyState(VK_SHIFT) < 0, g_key_capture.ctrl = GetKeyState(VK_CONTROL) < 0;
        g_key_capture.alt = GetKeyState(VK_MENU) < 0;
      }
      return 0;
    }
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
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// Writes the back buffer (RGBA8) as a 32-bit BMP.
bool SaveBackbuffer(const fs::path& out) {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  D3D11_TEXTURE2D_DESC d;
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  bool ok = false;
  if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &staging))) {
    g_context->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      std::error_code ec;
      fs::create_directories(out.parent_path(), ec);
      std::ofstream f(out, std::ios::binary);
      const uint32_t w = d.Width, h = d.Height, size = w * h * 4;
      BITMAPFILEHEADER fh{0x4D42, DWORD(54 + size), 0, 0, 54};
      BITMAPINFOHEADER ih{sizeof(ih), LONG(w), -LONG(h), 1, 32, BI_RGB, size};
      f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
      f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
      std::vector<uint8_t> row(w * 4);
      for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        for (uint32_t x = 0; x < w; ++x)
          row[4 * x] = src[4 * x + 2], row[4 * x + 1] = src[4 * x + 1], row[4 * x + 2] = src[4 * x],
          row[4 * x + 3] = 255;
        f.write(reinterpret_cast<const char*>(row.data()), row.size());
      }
      ok = bool(f);
      g_context->Unmap(staging, 0);
    }
    staging->Release();
  }
  back->Release();
  return ok;
}

ID3D11ShaderResourceView* MakeTexture(const std::vector<uint8_t>& rgba, int w, int h) {
  if (rgba.empty() || w <= 0 || h <= 0) return nullptr;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = UINT(w), d.Height = UINT(h), d.MipLevels = 1, d.ArraySize = 1, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{rgba.data(), UINT(w * 4), 0};
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g_device->CreateTexture2D(&d, &init, &tex))) return nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  g_device->CreateShaderResourceView(tex, nullptr, &srv);
  tex->Release();
  return srv;
}

std::optional<fs::path> PickFile(HWND owner) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  COMDLG_FILTERSPEC types[] = {{L"Videos (*.mp4, *.mov, *.mkv, *.bik2)", L"*.mp4;*.mov;*.mkv;*.bik2"},
                               {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, types);
  dlg->SetTitle(L"Pick a video for this tron");
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// Session 33: an audio file for a wrestler's entrance music.
std::optional<fs::path> PickAudio(HWND owner) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  COMDLG_FILTERSPEC types[] = {{L"Music (*.mp3, *.m4a, *.wav, *.flac, *.wma, *.aac)", L"*.mp3;*.m4a;*.wav;*.flac;*.wma;*.aac"},
                               {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, types);
  dlg->SetTitle(L"Pick this wrestler's entrance music");
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// Session 25: PNGs for the Logos page (several at once).
std::vector<fs::path> PickPngs(HWND owner) {
  std::vector<fs::path> out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  COMDLG_FILTERSPEC types[] = {{L"PNG pictures (*.png)", L"*.png"}};
  dlg->SetFileTypes(1, types);
  dlg->SetTitle(L"Pick PNG pictures to import as logos");
  FILEOPENDIALOGOPTIONS o = 0;
  dlg->GetOptions(&o);
  dlg->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->GetResults(&items))) {
      DWORD n = 0;
      items->GetCount(&n);
      for (DWORD i = 0; i < n; ++i) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item))) continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out.emplace_back(path), CoTaskMemFree(path);
        item->Release();
      }
      items->Release();
    }
  }
  dlg->Release();
  return out;
}

// Session 26: a disc image of the player's own disc as the game files.
std::optional<fs::path> PickDiscImage(HWND owner) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  COMDLG_FILTERSPEC types[] = {{L"Xbox 360 disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, types);
  dlg->SetTitle(L"Pick the disc image (.iso) of your WWE 2K14 disc");
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

std::optional<fs::path> PickFolder(HWND owner, const wchar_t* title) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  FILEOPENDIALOGOPTIONS o = 0;
  dlg->GetOptions(&o);
  dlg->SetOptions(o | FOS_PICKFOLDERS);
  dlg->SetTitle(title);
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// ---------------------------------------------------------------------------------------------------------------

// Stacked alpha (Session 18): `px` holds the colour half (4 bytes a pixel), `alpha_half` the bottom half; each pixel's
// alpha becomes the bottom half's grey (its green channel; the decoder already expanded it to full range).
inline void MergeStackedAlpha(uint8_t* px, const uint8_t* alpha_half, size_t pixels) {
  for (size_t i = 0; i < pixels; ++i) px[4 * i + 3] = alpha_half[4 * i + 1];
}

// The same for a whole RGBA picture (w x h, h = 2 x the picture): keeps the top half with the alpha merged in.
inline void UnstackRgba(std::vector<uint8_t>& rgba, int w, int& h) {
  if (h < 4 || rgba.size() < size_t(w) * h * 4) return;
  const int ph = h / 2;
  MergeStackedAlpha(rgba.data(), rgba.data() + size_t(w) * (h - ph) * 4, size_t(w) * ph);
  rgba.resize(size_t(w) * ph * 4);
  h = ph;
}

struct Thumb {
  fs::path file;
  fs::file_time_type stamp;
  VideoInfo info;
  ID3D11ShaderResourceView* srv = nullptr;
  int w = 0, h = 0;
  ~Thumb() { ReleaseLater(srv); }  // may be reset mid-frame (Assign / Revert)
};

// Plays one video into a texture (the hovered tile's preview). A background thread decodes ahead (up to 3 frames);
// the UI thread only shows the frame that is due (time stamps on the wall clock) and loops at the end. With the GPU
// path the frame is copied GPU-to-GPU into our texture; otherwise (no video support, or a format the GPU path refuses)
// the thread decodes in software and the UI uploads the pixels.
class Player {
 public:
  ~Player() { Close(); }
  const fs::path& File() const { return file_; }
  int W() const { return w_; }
  int H() const { return h_; }
  bool Playing() const { return running_; }
  bool Gpu() const { return gpu_; }

  bool Open(const fs::path& f) {
    Close();
    file_ = f;
    VideoInfo probe = MfInfo(f, 0.0, nullptr);  // size (and starts Media Foundation)
    if (!probe.ok) return false;
    stacked_ = IsStackedAlpha(f) && probe.h >= 4;  // Session 18: colour on top, alpha below -> one RGBA picture
    w_ = probe.w, h_ = stacked_ ? probe.h / 2 : probe.h, full_h_ = probe.h;
    stop_ = false, running_ = true;
    start_set_ = false, have_frame_ = false;
    thread_ = std::thread([this, f] { Decode(f); });
    return true;
  }

  void Close() {
    if (thread_.joinable()) {
      {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
      }
      cv_.notify_all();
      thread_.join();
    }
    for (auto& fr : queue_) fr.Free();
    queue_.clear();
    ReleaseLater(srv_), srv_ = nullptr;
    ReleaseLater(tex_), tex_ = nullptr;
    file_.clear();
    running_ = false, have_frame_ = false, gpu_ = false;
  }

  // Shows the frame that is due now; returns the texture once it holds a frame.
  ID3D11ShaderResourceView* Tick() {
    if (!running_) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    Frame show;
    bool got = false;
    {
      std::lock_guard<std::mutex> l(mu_);
      while (!queue_.empty()) {
        Frame& front = queue_.front();
        if (front.loop) {  // the video ended and restarted: its time stamps start again
          if (got) break;  // show the last frame first
          start_set_ = false;
          queue_.pop_front();
          continue;
        }
        if (!start_set_) start_ = now - std::chrono::microseconds(front.ts / 10), start_set_ = true;
        const LONGLONG elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_).count() * 10;
        if (front.ts > elapsed && (got || have_frame_)) break;  // not due yet
        if (got) show.Free();
        show = std::move(front), got = true;
        queue_.pop_front();
      }
    }
    cv_.notify_all();  // room in the queue
    if (got) {
      Present(show);
      show.Free();
    }
    return have_frame_ ? srv_ : nullptr;
  }

 private:
  struct Frame {
    LONGLONG ts = 0;
    bool loop = false;
    IMFSample* sample = nullptr;  // GPU path: the decoder's texture lives in the sample
    std::vector<uint8_t> px;      // software path: BGRA rows, w_ * 4 apart
    void Free() {
      if (sample) sample->Release(), sample = nullptr;
      px.clear();
    }
    Frame() = default;
    Frame(Frame&& o) noexcept : ts(o.ts), loop(o.loop), sample(o.sample), px(std::move(o.px)) { o.sample = nullptr; }
    Frame& operator=(Frame&& o) noexcept {
      if (this != &o) Free(), ts = o.ts, loop = o.loop, sample = o.sample, px = std::move(o.px), o.sample = nullptr;
      return *this;
    }
  };

  bool EnsureTexture(DXGI_FORMAT fmt) {
    if (tex_ && fmt_ == fmt) return true;
    ReleaseLater(srv_), srv_ = nullptr;
    ReleaseLater(tex_), tex_ = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w_), d.Height = UINT(h_), d.MipLevels = 1, d.ArraySize = 1, d.Format = fmt;
    d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &tex_))) return false;
    fmt_ = fmt;
    return SUCCEEDED(g_device->CreateShaderResourceView(tex_, nullptr, &srv_));
  }

  void Present(Frame& f) {
    if (f.sample) {  // GPU: copy the decoder's texture into ours
      IMFMediaBuffer* b = nullptr;
      IMFDXGIBuffer* db = nullptr;
      ID3D11Texture2D* src = nullptr;
      UINT sub = 0;
      if (SUCCEEDED(f.sample->GetBufferByIndex(0, &b)) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&db))) &&
          SUCCEEDED(db->GetResource(IID_PPV_ARGS(&src))) && SUCCEEDED(db->GetSubresourceIndex(&sub))) {
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        if (EnsureTexture(sd.Format)) {
          const D3D11_BOX box{0, 0, 0, UINT(std::min<int>(w_, int(sd.Width))), UINT(std::min<int>(h_, int(sd.Height))), 1};
          g_context->CopySubresourceRegion(tex_, 0, 0, 0, 0, src, sub, &box);
          have_frame_ = true;
          LogPreviewPixel(tex_, f.ts, true, w_, h_);
        }
      }
      if (src) src->Release();
      if (db) db->Release();
      if (b) b->Release();
    } else if (!f.px.empty() && EnsureTexture(DXGI_FORMAT_B8G8R8A8_UNORM)) {  // software: upload the pixels
      g_context->UpdateSubresource(tex_, 0, nullptr, f.px.data(), UINT(w_) * 4, 0);
      have_frame_ = true;
      LogPreviewPixel(tex_, f.ts, false, w_, h_);
    }
  }

  IMFSourceReader* MakeReader(const fs::path& f, bool gpu) {
    IMFAttributes* a = nullptr;
    MFCreateAttributes(&a, 3);
    if (gpu) {
      a->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g_dxgi_manager);
      a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
      a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    } else {
      a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(f.wstring().c_str(), a, &r);
    a->Release();
    if (FAILED(hr)) return nullptr;
    r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    // GPU: B8G8R8A8 (opaque: alpha 255). Software: RGB32 (the basic converter has no ARGB32; Session 18), alpha set by us.
    t->SetGUID(MF_MT_SUBTYPE, gpu ? MFVideoFormat_ARGB32 : MFVideoFormat_RGB32);
    hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
    t->Release();
    if (FAILED(hr)) return r->Release(), nullptr;
    return r;
  }

  // The decode thread: reads ahead, keeps at most 3 frames queued, rewinds at the end.
  void Decode(fs::path f) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMFSourceReader* r = g_dxgi_manager && !stacked_ ? MakeReader(f, true) : nullptr;  // stacked: CPU (merge)
    gpu_ = r != nullptr;
    if (!r) r = MakeReader(f, false);
    if (!r) {
      running_ = false;
      CoUninitialize();
      return;
    }
    for (;;) {
      {
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait(l, [this] { return stop_ || queue_.size() < 3; });
        if (stop_) break;
      }
      DWORD flags = 0;
      LONGLONG ts = 0;
      IMFSample* s = nullptr;
      if (FAILED(r->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
      Frame fr;
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = 0;
        r->SetCurrentPosition(GUID_NULL, pos);
        fr.loop = true;
      } else if (!s) {
        continue;
      } else {
        fr.ts = ts;
        IMFMediaBuffer* b = nullptr;
        IMFDXGIBuffer* db = nullptr;
        if (gpu_ && SUCCEEDED(s->GetBufferByIndex(0, &b)) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&db)))) {
          fr.sample = s, s = nullptr;  // the UI copies it (GPU-to-GPU)
        } else {
          IMFMediaBuffer* cb = nullptr;  // software path: BGRA bytes for the UI to upload
          if (SUCCEEDED(s->ConvertToContiguousBuffer(&cb))) {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(cb->Lock(&data, nullptr, &len))) {
              if (stacked_ && len >= DWORD(w_ * full_h_ * 4)) {
                fr.px.assign(data, data + size_t(w_) * h_ * 4);
                MergeStackedAlpha(fr.px.data(), data + size_t(w_) * (full_h_ - h_) * 4, size_t(w_) * h_);
              } else if (len >= DWORD(w_ * h_ * 4)) {
                fr.px.assign(data, data + size_t(w_) * h_ * 4);
                for (size_t i = 3; i < fr.px.size(); i += 4) fr.px[i] = 255;  // RGB32's fourth byte is undefined
              }
              cb->Unlock();
            }
            cb->Release();
          }
        }
        if (db) db->Release();
        if (b) b->Release();
        if (s) s->Release();
      }
      {
        std::lock_guard<std::mutex> l(mu_);
        queue_.push_back(std::move(fr));
      }
      WakeUi();
    }
    r->Release();
    CoUninitialize();
  }

  fs::path file_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Frame> queue_;
  bool stop_ = false;
  std::atomic<bool> running_{false}, gpu_{false};
  ID3D11Texture2D* tex_ = nullptr;
  ID3D11ShaderResourceView* srv_ = nullptr;
  DXGI_FORMAT fmt_ = DXGI_FORMAT_UNKNOWN;
  int w_ = 0, h_ = 0, full_h_ = 0;
  bool stacked_ = false;
  std::chrono::steady_clock::time_point start_;
  bool start_set_ = false, have_frame_ = false;
};

// Thumbnails decoded on a worker thread (PNG with stb_image; the first frame of an assigned video with Media
// Foundation); the UI thread only turns finished pictures into textures.
class ThumbLoader {
 public:
  struct Done {
    int tron = 0;
    fs::path file;
    fs::file_time_type stamp;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
  };
  ~ThumbLoader() {
    {
      std::lock_guard<std::mutex> l(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  void Request(int tron, const fs::path& f, fs::file_time_type stamp) {
    std::lock_guard<std::mutex> l(mu_);
    if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
    for (const auto& q : todo_)
      if (q.tron == tron && q.file == f) return;
    todo_.push_back({tron, f, stamp, {}, 0, 0});
    cv_.notify_all();
  }
  std::vector<Done> TakeDone() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<Done> out;
    out.swap(done_);
    return out;
  }
  bool Busy() {
    std::lock_guard<std::mutex> l(mu_);
    return !todo_.empty() || working_;
  }

 private:
  void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
      Done job;
      {
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait(l, [this] { return stop_ || !todo_.empty(); });
        if (stop_) break;
        job = std::move(todo_.front());
        todo_.pop_front();
        working_ = true;
      }
      if (Lower(PathStr(job.file.extension())) == ".png") {
        int n = 0;
        if (FILE* fp = _wfopen(job.file.c_str(), L"rb")) {
          if (unsigned char* px = stbi_load_from_file(fp, &job.w, &job.h, &n, 4)) {
            job.rgba.assign(px, px + size_t(job.w) * job.h * 4);
            stbi_image_free(px);
          }
          std::fclose(fp);
        }
      } else {
        VideoInfo vi = MfInfo(job.file, 0.0, &job.rgba);
        job.w = vi.w, job.h = vi.h;
        if (IsStackedAlpha(job.file)) UnstackRgba(job.rgba, job.w, job.h);
      }
      {
        std::lock_guard<std::mutex> l(mu_);
        done_.push_back(std::move(job));
        working_ = false;
      }
      WakeUi();
    }
    CoUninitialize();
  }
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Done> todo_;
  std::vector<Done> done_;
  bool stop_ = false, working_ = false;
};

struct Step {
  std::string op, a, b;
};

class App {
 public:
  explicit App(HWND hwnd) : hwnd_(hwnd) {}

  Settings s;
  fs::path settings_path;
  bool save_settings = true;
  std::deque<Step> steps;
  fs::path log_path;
  bool exit_after_steps = false;

  void Load() {
    if (settings_path.empty()) settings_path = SettingsPath();
    s = LoadSettings(settings_path);
    if (!override_game_data.empty()) s.game_data_dir = override_game_data;
    if (!override_user_data.empty()) s.user_data_dir = override_user_data;
    hw_ = DetectHardware(hwnd_);
    if (hw_.refresh_hz > 0) g_monitor_hz = hw_.refresh_hz;
    if (s.first_run_done && s.loaded_version < 3 && MigrateForDevice(s) && save_settings) SaveSettings(settings_path, s);
    g_perf_report_default = IsPlaytest();
    Reload();
    StartUpdateCheck(update_force_);
  }

  // ---------------- Session 31: update check (update_check.h). In the background at start, unless switched off in
  // Settings; development builds (+dev) skip it unless --update-check. --update-file JSON reads a release from a file
  // instead of GitHub and --update-version VER pretends this version (tests).
  struct UpdateState {
    std::mutex m;
    UpdateInfo info;
  };
  std::shared_ptr<UpdateState> update_ = std::make_shared<UpdateState>();
  bool update_force_ = false, update_dismissed_ = false, want_update_popup_ = false;
  std::string update_file_, update_version_;

  std::string MyVersion() const { return update_version_.empty() ? std::string(Version()) : update_version_; }
  UpdateInfo Update() {
    std::lock_guard<std::mutex> lock(update_->m);
    return update_->info;
  }
  void StartUpdateCheck(bool force) {
    const bool dev = MyVersion().find("+dev") != std::string::npos;
    if (!force && (!s.check_updates || dev)) return;
    {
      std::lock_guard<std::mutex> lock(update_->m);
      if (update_->info.state == UpdateInfo::kChecking) return;
      update_->info = {}, update_->info.state = UpdateInfo::kChecking;
    }
    std::thread([st = update_, ver = MyVersion(), file = update_file_] {
      UpdateInfo u;
      if (file.empty()) {
        u = CheckForUpdate(ver);
      } else {
        std::ifstream f(fs::path(Widen(file)), std::ios::binary);
        u = ReadRelease(std::string((std::istreambuf_iterator<char>(f)), {}), ver);
      }
      {
        std::lock_guard<std::mutex> lock(st->m);
        st->info = u;
      }
      WakeUi();
    }).detach();  // owns its state (shared_ptr): never joined, so closing the launcher never waits for GitHub
  }
  void LogUpdateOnce() {
    static UpdateInfo::State logged = UpdateInfo::kNone;
    const UpdateInfo u = Update();
    if (u.state == logged || u.state == UpdateInfo::kChecking) return;
    logged = u.state;
    Log("update check: " + std::string(u.state == UpdateInfo::kAvailable ? "available " + u.tag + " " + u.url
                                       : u.state == UpdateInfo::kLatest  ? "up to date (" + MyVersion() + ")"
                                       : u.state == UpdateInfo::kFailed  ? "failed: " + u.error
                                                                         : "not run"));
  }

  // The notice at the band's foot (above Report a problem) when a newer release exists; a click opens the details.
  float UpdateNotice(ImVec2 pos, float width) {
    const UpdateInfo u = Update();
    if (u.state != UpdateInfo::kAvailable || update_dismissed_) return 0;
    const float d = Dpi(), h = 46 * d;
    const std::string label = "UPDATE " + u.tag.substr(0, u.tag.find('-'));
    if (SlantButton(label.c_str(), pos, ImVec2(width, h), true, 17 * d)) want_update_popup_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A newer version is out: see what's new");
    return h;
  }
  void UpdatePopup() {
    if (want_update_popup_) ImGui::OpenPopup("update"), want_update_popup_ = false;
    if (!BeginGameModal("update", "UPDATE AVAILABLE", 660 * Dpi())) return;
    const UpdateInfo u = Update();
    ImGui::TextWrapped("%s is out (you have %s).", u.name.empty() ? u.tag.c_str() : u.name.c_str(), MyVersion().c_str());
    ImGui::TextDisabled("Download it from its GitHub page; your saves, settings and mods stay where they are.");
    if (!u.notes.empty()) {
      ImGui::Spacing();
      ImGui::BeginChild("notes", ImVec2(0, 240 * Dpi()), ImGuiChildFlags_Borders);
      ImGui::TextWrapped("%s", u.notes.c_str());
      ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, 10 * Dpi()));
    if (ModalMainButton("OPEN THE RELEASE PAGE")) {
      if (u.url.rfind("https://github.com/", 0) == 0) ShellExecuteW(hwnd_, L"open", Widen(u.url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      ImGui::CloseCurrentPopup();
    }
    if (ModalButton("Later")) update_dismissed_ = true, ImGui::CloseCurrentPopup();
    ImGui::NewLine();
    ImGui::EndPopup();
  }

  // The first-run wizard (unless automation said otherwise: it would cover every screenshot).
  void MaybeWizard(bool automation) {
    if (wizard_mode_ == 1 || (wizard_mode_ == 0 && !automation && !s.first_run_done)) want_wizard_ = true;
  }
  int wizard_mode_ = 0;  // --wizard 1, --no-wizard -1
  std::string override_game_data, override_user_data;  // --game-data / --user-data (this run only)
  void OpenAdvanced() { advanced_open_ = true; }
  void ForceRtx() { force_rtx_ = true; }
  void FakeIntegratedGpu() { fake_igpu_ = true; }  // Session 37: --fake-igpu
  void FakeNoInterlock() { fake_no_interlock_ = true; }  // Session 54: --fake-no-interlock
  void NoOpen() { open_folders_ = false; }

  void Reload() {
    paths_ = FindPaths(s);
    std::string err;
    chars_ = LoadCharacters(paths_.ids, err);
    if (!err.empty()) Status("characters: " + err);
    arenas_ = LoadArenas(paths_.ids);
    scenarios_ = LoadScenarios(paths_);
    replays_ = ListFiles(paths_.replays, ".rec", true);
    directors_ = ListFiles(paths_.director, ".txt", true);
    shots_ = ListFiles(paths_.shots, ".txt", false);
    routes_ = ListFiles(paths_.routes, ".txt", false);
    for (const auto& r : ListFiles(paths_.routes / "short", ".txt", false)) routes_.push_back("short\\" + r);
    RefreshOverrides();
    seeds_scenario_.clear();
    CheckGameDataNow();
    logo_loaded_ = false;
    LoadMenuFont();
  }

  // Session 31: the game's menu face from the player's own game files (game_font.h), again when they change.
  void LoadMenuFont() {
    if (gfont_tried_ && gfont_root_ == paths_.game_data) return;
    gfont_tried_ = true, gfont_root_ = paths_.game_data;
    ReleaseLater(gfont_srv_), gfont_srv_ = nullptr;
    if (!LoadGameFont(GameFilesAt(paths_.game_data), gfont_)) {
      Log("menu font: not found in the game files; using the fallback font");
      return;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(gfont_.sheet_w), d.Height = UINT(gfont_.sheet_h), d.MipLevels = 1, d.ArraySize = 1;
    d.Format = DXGI_FORMAT_BC3_UNORM, d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{gfont_.bc3.data(), UINT(gfont_.sheet_w / 4 * 16), 0};
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, &init, &tex))) {
      g_device->CreateShaderResourceView(tex, nullptr, &gfont_srv_);
      tex->Release();
    }
    gfont_.bc3 = {};
    Log(gfont_srv_ ? "menu font: the game's font000 (" + std::to_string(gfont_.glyphs.size()) + " glyphs)"
                   : "menu font: texture failed; using the fallback font");
  }

  void RefreshOverrides() { overrides_ = AllOverrides(paths_); }

  void Status(const std::string& msg) {
    status_ = msg;
    status_time_ = std::chrono::steady_clock::now();
    Log(msg);
  }

  void Log(const std::string& msg) {
    // Session 58: the last 300 messages in memory too, for Report a problem (players have no --log file).
    recent_log_.push_back(NowStamp("%Y-%m-%d %H:%M:%S") + "  " + msg);
    if (recent_log_.size() > 300) recent_log_.erase(recent_log_.begin());
    if (log_path.empty()) return;
    std::ofstream f(log_path, std::ios::app);
    f << msg << "\n";
  }

  // Runs one automation step (the same code the buttons use). Returns false when the step must wait a frame.
  bool RunStep(const Step& st) {
    if (st.op == "tab") {  // Sessions 17-24 tab names
      if (st.a == "play") s.page = 0;
      else if (st.a == "settings") s.page = 2;
      else s.page = 1, want_mods_tab_ = st.a == "titles" ? 1 : st.a == "logos" ? 2 : 0;
    } else if (st.op == "page") {  // Session 31: + about, quick (Play -> Quick Match), advanced (Play -> Advanced)
      GoPage(st.a == "mods" ? 1 : st.a == "settings" ? 2 : st.a == "about" ? 3 : 0);
      play_view_ = st.a == "quick" ? 1 : st.a == "advanced" ? 2 : st.a == "wm30" ? 3 : 0;
    }
    else if (st.op == "mods_tab") s.page = 1, want_mods_tab_ = st.a == "titles" ? 1 : st.a == "logos" ? 2 : 0;
    else if (st.op == "logo_add") {  // PNG or PNG@SLOT (a test: that slot, 1-20, instead of the first free one)
      const size_t at = st.a.rfind('@');
      const bool forced = at != std::string::npos && at + 1 < st.a.size() &&
                          std::all_of(st.a.begin() + std::ptrdiff_t(at) + 1, st.a.end(), ::isdigit);
      const fs::path f = fs::path(Widen(forced ? st.a.substr(0, at) : st.a));
      AddLogoPick(f);
      if (forced) logo_forced_[f] = std::atoi(st.a.c_str() + at + 1) - 1;
    }
    else if (st.op == "logo_import") StartLogoImport();
    else if (st.op == "logo_size") s.logo_size = std::atoi(st.a.c_str()) == 128 ? 128 : 256;
    else if (st.op == "wait_game") return game_proc_ == nullptr;
    else if (st.op == "perf_test") StartPerfTest();                // Session 39
    else if (st.op == "wait_perf_test") return perf_step_ < 0;     // ... until all its runs are done
    else if (st.op == "show_perf_test") want_perf_popup_ = true;   // its window, for a look
    else if (st.op == "backup") DoBackup("automation");
    else if (st.op == "restore") RefreshLogos(true), DoRestore(std::atoi(st.a.c_str()));
    else if (st.op == "report") MakeReport();
    else if (st.op == "play") LaunchPlain();
    else if (st.op == "create_shortcut") CreateDesktopShortcut();  // Session 53 (with --shortcut-dir in tests)
    else if (st.op == "build_previews") BuildPreviews();
    else if (st.op == "wizard_step") wizard_step_ = std::clamp(std::atoi(st.a.c_str()), 0, 2);
    else if (st.op == "no_rtx") hw_.rtx = false, force_rtx_ = false;
    else if (st.op == "quick_match") LaunchQuickMatch();
    else if (st.op == "quick_type") s.quick_mode = 0, s.quick_type = std::atoi(st.a.c_str());  // Session 31
    else if (st.op == "wm30_era") wm30_era_ = std::clamp(std::atoi(st.a.c_str()), 0, 4), wm30_sel_ = -1;  // Session 34
    else if (st.op == "wm30_play") {  // Session 34: what PLAY MATCH does for match N (the list read first)
      if (!wm30_.loaded && gd_.ok) wm30_ = wwe2k14::wm30list::Load(GameFilesAt(paths_.game_data));
      const int n = std::atoi(st.a.c_str());
      for (const auto& m : wm30_.matches)
        if (m.index == n) LaunchWm30(m);
    }
    else if (st.op == "settings_section") s.page = 2, settings_section_ = std::atoi(st.a.c_str());
    else if (st.op == "show_crash") crash_code_ = DWORD(std::strtoul(st.a.c_str(), nullptr, 0)), want_crash_popup_ = true;
    else if (st.op == "show_report") want_report_popup_ = true;
    else if (st.op == "show_keys") want_keys_popup_ = true;  // Session 67
    else if (st.op == "keys_capture") BeginCapture(std::clamp(std::atoi(st.a.c_str()), 0, keys::kBindCount - 1), 0);
    else if (st.op == "keys_hot") key_hot_force_ = std::atoi(st.a.c_str());
    else if (st.op == "titles_view") s.titles_view = st.a == "grid" ? 1 : 0;
    else if (st.op == "show_update") want_update_popup_ = Update().state == UpdateInfo::kAvailable;
    else if (st.op == "capture") LaunchCapture(st.a);
    else if (st.op == "wait_game_start") return game_proc_ == nullptr || WaitForInputIdle(game_proc_, 0) != WAIT_TIMEOUT;
    else if (st.op == "kill_game") {  // a test of the crash dialog: end the game with this exit code
      if (game_proc_) TerminateProcess(game_proc_, UINT(std::strtoul(st.a.c_str(), nullptr, 0)));
    }
    else if (st.op == "wait") {}
    else if (st.op == "banner") DoBanner(std::atoi(st.a.c_str()), st.b);
    else if (st.op == "title") title_sel_ = std::atoi(st.a.c_str());
    else if (st.op == "assign_alpha") DoAssign(std::atoi(st.a.c_str()), fs::path(Widen(st.b)), true);
    else if (st.op == "select") Select(std::atoi(st.a.c_str()));
    else if (st.op == "assign") DoAssign(std::atoi(st.a.c_str()), fs::path(Widen(st.b)));
    else if (st.op == "revert") DoRevert(std::atoi(st.a.c_str()));
    else if (st.op == "scenario") s.scenario = st.a;
    else if (st.op == "mods") s.mods = st.a != "0";
    else if (st.op == "harness") s.harness = true, s.harness_route = st.a;
    else if (st.op == "preset") {
      for (int i = 0; i < kPresetCount; ++i)
        if (Lower(kPresetNames[i]) == Lower(st.a) || (st.a == "reshoot" && i == kReshoot)) s.preset = i;
    } else if (st.op == "launch") DoLaunch(false);
    else if (st.op == "view") s.trons_view = st.a == "grid" ? 1 : 0;
    else if (st.op == "sort") s.trons_sort = std::abs(std::atoi(st.a.c_str())) % 4, s.trons_sort_desc = st.a[0] == '-';
    else if (st.op == "reset_play") ResetPlay();
    else if (st.op == "hover") force_hover_ = std::atoi(st.a.c_str());
    else if (st.op == "shot") Log(SaveBackbuffer(fs::path(Widen(st.a))) ? "shot " + st.a : "shot FAILED " + st.a);
    return true;
  }

  void Frame() {
    CollectThumbs();
    PollGame();
    DirectStartPoll();  // Session 53
    hover_video_.clear();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    // Session 31: the background picture (dots, the red panel), the page list on the panel, the page to its right.
    const theme::Panel panel = NavGeometry(vp->WorkPos, vp->WorkSize);
    DrawBackground(vp->WorkPos, vp->WorkSize, panel);
    NavPanel(panel);
    ImGui::SetCursorScreenPos(ImVec2(panel.x0 + panel.w + 24 * Dpi(), vp->WorkPos.y));
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (s.page == 1) ModsPage();
    else if (s.page == 2) SettingsPage();
    else if (s.page == 3) AboutPage();
    else PlayPage();
    // Status line at the bottom.
    if (!status_.empty() && std::chrono::steady_clock::now() - status_time_ < std::chrono::seconds(12)) {
      ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 4);
      ImGui::Separator();
      ImGui::TextUnformatted(status_.c_str());
    }
    ImGui::EndChild();
    Wizard();
    CrashPopup();
    QuickStartPopup();  // Session 53
    PerfTestPopup();  // Session 39
    UpdatePopup();
    KeysPopup();  // Session 67
    LogUpdateOnce();
    ImGui::End();
    if (hover_video_.empty()) player_.Close();  // nothing hovered: stop playing
  }

  // Something moving: a preview playing or thumbnails loading (the main loop keeps drawing at the screen rate).
  bool Animating() { return player_.Playing() || thumbs_.Busy() || quick_pending_ || cap_bind_ >= 0; }

  // ---------------- Session 53 (items 2.1 / 2.4): the game without the launcher's window. `--start-game` (wwe2k14.exe
  // started directly hands over to it, src\direct_start.h; a desktop shortcut) starts the game at once, as PLAY GAME
  // does, and the window stays hidden; QUICK START (Settings -> GAME) does the same after 3 seconds in which a key or a
  // click keeps the launcher open (Shift held while it opens skips it). Hidden, the launcher closes when the game
  // closes, or shows itself when the game crashed (the crash window) or when something needs the player (no game
  // files yet, the game already running).
  bool start_game_ = false, quick_pending_ = false, exit_with_game_ = false;
  std::chrono::steady_clock::time_point quick_at_{};
  static constexpr int kQuickStartSeconds = 3;

  void BeginDirectStart(bool automation) {
    if (start_game_) {
      if (gd_.ok) {
        want_wizard_ = false;  // the files are there: the player asked for the game, not the wizard
        LaunchPlain();
        if (game_proc_) {
          exit_with_game_ = true;
          Log("direct start: the game started, the window stays hidden");
          return;
        }
      }
      ShowWindow(hwnd_, SW_SHOWDEFAULT);
      if (!gd_.ok) GoPage(2), settings_section_ = 0, Status("Pick your game files here, then press PLAY GAME.");
      Log(std::string("direct start: window shown (") + (gd_.ok ? "the game did not start" : "no game files") + ")");
      return;
    }
    if (s.quick_start && !automation && gd_.ok && !want_wizard_ && !GameRunning()) {
      if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        Status("Quick start skipped (Shift held).");
        return;
      }
      quick_pending_ = true;  // the 3 seconds start with the first frame drawn (the window takes ~2 s to come up)
      Log("quick start: counting down");
    }
  }

  // The countdown over the launcher (QUICK START); any key or click cancels it.
  void QuickStartPopup() {
    if (!quick_pending_) return;
    if (quick_at_ == std::chrono::steady_clock::time_point{}) {
      quick_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(kQuickStartSeconds);
      Log("quick start: countdown shown");
    }
    if (!ImGui::IsPopupOpen("quickstart")) ImGui::OpenPopup("quickstart");
    if (!BeginGameModal("quickstart", "STARTING THE GAME", 560 * Dpi())) return;
    const double left = std::chrono::duration<double>(quick_at_ - std::chrono::steady_clock::now()).count();
    ImGui::Text("The game starts in %d...", std::max(1, int(std::ceil(left))));
    ImGui::TextDisabled("Press any key or click to stay in the launcher. QUICK START is in Settings -> GAME.");
    ImGui::Dummy(ImVec2(0, 10 * Dpi()));
    bool stay = ModalMainButton("STAY IN THE LAUNCHER");
    ImGui::NewLine();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !stay; ++k)
      if (ImGui::IsKeyPressed(ImGuiKey(k), false)) stay = true;
    if (stay) {
      quick_pending_ = false;
      ImGui::CloseCurrentPopup();
      Status("Quick start cancelled: you are in the launcher.");
    } else if (left <= 0) {
      quick_pending_ = false;
      ImGui::CloseCurrentPopup();
      LaunchPlain();
      if (game_proc_) exit_with_game_ = true, ShowWindow(hwnd_, SW_HIDE), Log("quick start: the game started");
    }
    ImGui::EndPopup();
  }

  // Each frame: a hidden launcher closes with the game, or comes back for a crash.
  void DirectStartPoll() {
    if (!exit_with_game_ || game_proc_) return;
    exit_with_game_ = false;
    if (want_crash_popup_) {
      ShowWindow(hwnd_, SW_SHOWDEFAULT), SetForegroundWindow(hwnd_);
      Log("direct start: the game crashed, window shown");
      return;
    }
    Log("direct start: the game closed, the launcher closes too");
    PostQuitMessage(0);
  }

  // Settings -> GAME: a shortcut on the desktop (or in --shortcut-dir, tests) that starts the game directly.
  fs::path shortcut_dir_;
  bool CreateDesktopShortcut() {
    fs::path dir = shortcut_dir_;
    if (dir.empty()) {
      PWSTR desk = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) dir = desk;
      CoTaskMemFree(desk);
    }
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    IShellLinkW* link = nullptr;
    if (dir.empty() || FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
      return false;
    std::wstring args = L"--start-game";
    if (settings_path != SettingsPath()) args += L" --settings \"" + settings_path.wstring() + L"\"";
    link->SetPath(self);
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(fs::path(self).parent_path().c_str());
    link->SetIconLocation(paths_.game_exe.c_str(), 0);
    link->SetDescription(L"WWE 2K14 (starts the game with your launcher settings)");
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
    const fs::path lnk = dir / L"WWE 2K14.lnk";
    if (ok) ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE)), file->Release();
    link->Release();
    Status(ok ? "Made a desktop shortcut: " + PathStr(lnk) : "Couldn't make the shortcut in " + PathStr(dir));
    return ok;
  }

  void Shutdown() {
    if (game_proc_) CloseHandle(game_proc_), game_proc_ = nullptr;
    RECT r;
    if (GetWindowRect(hwnd_, &r)) s.window_w = r.right - r.left, s.window_h = r.bottom - r.top;
    if (save_settings) SaveSettings(settings_path, s);
    thumb_.reset();
  }

 private:
  // ================================================================================================================
  // Session 25: the launcher for players. A sidebar (Play Game, Mods, Settings; Report a problem and the version at
  // the bottom), the first-run wizard, crash detection, and the Logos page (src\launcher\logo_save.h).

  static constexpr ImVec4 kGood{0.45f, 0.85f, 0.45f, 1}, kBad{1, 0.55f, 0.4f, 1}, kWarn{1, 0.8f, 0.35f, 1};

  float Dpi() const { return ImGui::GetStyle().FontScaleDpi; }

  // A heading in the menu face (an ImGui item, so the layout flows around it).
  void Heading(const char* text, float scale = 1.35f) {
    const float px = ImGui::GetStyle().FontSizeBase * scale * Dpi();
    const ImVec2 pos = ImGui::GetCursorScreenPos(), sz = MenuTextSize(px, text);
    MenuText(ImGui::GetWindowDrawList(), pos, px, IM_COL32_WHITE, text);
    ImGui::Dummy(sz);
  }

  void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
  }

  // A big coloured button (Play, Import).
  bool BigButton(const char* label, ImVec2 size, ImVec4 col = ImVec4(0.55f, 0.11f, 0.13f, 1)) {  // S31: red, was green
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x * 1.25f, col.y * 1.25f, col.z * 1.25f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x * 1.4f, col.y * 1.4f, col.z * 1.4f, 1));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    const bool r = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    return r;
  }

  // ---------------- Session 31: the game's menu look (launcher_theme.h)
  static constexpr ImU32 kMenuRed = IM_COL32(214, 22, 30, 255);
  // The page list, top to bottom, and each item's s.page (0 Play, 1 Mods, 2 Settings, 3 About: the stored numbers
  // stay as before Session 31).
  static constexpr int kNavPages[4] = {0, 2, 1, 3};
  static constexpr const char* kNavNames[4] = {"PLAY", "SETTINGS", "MODS", "ABOUT"};
  static constexpr float kNavPad = 18, kNavRow = 52;

  void GoPage(int page) {
    s.page = page;
    if (page == 0) play_view_ = 0;  // PLAY always opens its home screen
  }

  // The red band (Session 31, Jarrod: full height, "definitely most finished looking", over the floating panel): from
  // the window's top to its bottom on the left edge, the right side leaning like the game's panels.
  theme::Panel NavGeometry(ImVec2 origin, ImVec2 size) const {
    const float d = Dpi();
    theme::Panel p;
    p.x0 = origin.x, p.y0 = origin.y, p.w = 310 * d, p.h = std::max(size.y, 1.f), p.straight_left = true;
    p.slant = std::min(p.h * 0.16f, p.w - 210 * d);  // the foot stays wide enough for the footer
    return p;
  }

  // One picture for the whole window (dots + the red panel), remade only when the window size changes.
  void DrawBackground(ImVec2 pos, ImVec2 size, const theme::Panel& p) {
    const int w = int(size.x), h = int(size.y);
    if (w <= 0 || h <= 0) return;
    if (!bg_srv_ || w != bg_w_ || h != bg_h_) {
      theme::Panel local = p;
      local.x0 -= pos.x, local.y0 -= pos.y;
      ReleaseLater(bg_srv_);
      bg_srv_ = MakeTexture(theme::Background(w, h, Dpi(), local), w, h);
      bg_w_ = w, bg_h_ = h;
    }
    if (bg_srv_)
      ImGui::GetWindowDrawList()->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(bg_srv_)), pos,
                                           ImVec2(pos.x + float(w), pos.y + float(h)));
  }

  // The band: the logo at the top, the pages under it (stepping down the slant, the selected one a black bar with red
  // text: the game's PLAY row; a hovered one a faint bar), the footer at the foot (game running, Report a problem,
  // the version).
  void NavPanel(const theme::Panel& p) {
    const float d = Dpi(), margin = 20 * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 logo = DrawLogo(dl, ImVec2(p.x0 + margin, p.y0 + 18 * d), p.w - 3 * margin, 2);
    const float top = p.y0 + 18 * d + (logo.y > 0 ? logo.y + 24 * d : 0);
    for (int i = 0; i < 4; ++i) {
      const float y0 = top + kNavRow * d * float(i), y1 = y0 + kNavRow * d, yc = (y0 + y1) / 2;
      ImGui::SetCursorScreenPos(ImVec2(p.Left(y0), y0));
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##nav", ImVec2(p.Right(y0) - p.Left(y0), y1 - y0))) GoPage(kNavPages[i]);
      ImGui::PopID();
      const bool on = s.page == kNavPages[i], hover = ImGui::IsItemHovered();
      if (on || hover) {
        const ImVec2 bar[4] = {{p.Left(y0), y0}, {p.Right(y0), y0}, {p.Right(y1), y1}, {p.Left(y1), y1}};
        dl->AddConvexPolyFilled(bar, 4, on ? IM_COL32(0, 0, 0, 255) : IM_COL32(0, 0, 0, 80));
      }
      // Each row starts a little further left, following the right edge's slant (the game's staircase).
      const float px = 34 * d, tx = p.x0 + 2.4f * margin - (p.Right(top) - p.Right(yc));
      const ImVec2 at(tx, yc - MenuTextSize(px, kNavNames[i]).y * 0.5f);
      if (on) MenuTextRaw(dl, at, px, kMenuRed, kNavNames[i]);
      else MenuText(dl, at, px, IM_COL32_WHITE, kNavNames[i]);
    }
    dl->AddLine(ImVec2(p.x0 + p.w, p.y0), ImVec2(p.x0 + p.w - p.slant, p.y0 + p.h), IM_COL32(255, 255, 255, 170),
                1.6f * d);  // the game's light rim on the right edge

    // Footer, bottom up, inside the band's foot.
    const float fw = p.Right(p.y0 + p.h) - p.x0 - 2 * margin, line = ImGui::GetTextLineHeight(), gap = 6 * d;
    float y = p.y0 + p.h - 16 * d - line;
    ImGui::SetCursorScreenPos(ImVec2(p.x0 + margin, y));
    ImGui::TextColored(ImVec4(1, 1, 1, 0.7f), "%s", Version());
    y -= ImGui::GetFrameHeight() + gap;
    ImGui::SetCursorScreenPos(ImVec2(p.x0 + margin, y));
    if (ImGui::Button("Report a problem", ImVec2(fw, 0)) || want_report_popup_)
      ImGui::OpenPopup("report"), want_report_popup_ = false;
    ReportPopup();
    {  // Session 31: the update notice, when there is one
      const float uh = 46 * d;
      if (UpdateNotice(ImVec2(p.x0 + margin, y - gap - uh), fw) > 0) y -= uh + gap;
    }
    if (game_proc_) {
      const float px = 17 * d;
      MenuText(dl, ImVec2(p.x0 + margin, y - px - gap), px, IM_COL32_WHITE, "GAME RUNNING");
    }
  }

  // A slanted button (the PLAY GAME / QUICK MATCH tiles): red, white rim, the label in the menu font, shrunk to fit.
  bool SlantButton(const char* label, ImVec2 pos, ImVec2 size, bool enabled, float font_px) {
    const float d = Dpi(), sk = size.y * 0.22f;
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    ImGui::EndDisabled();
    const bool hover = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    ImU32 top = IM_COL32(160, 20, 28, 255), bottom = IM_COL32(82, 8, 12, 255);
    if (!enabled) top = IM_COL32(66, 66, 72, 255), bottom = IM_COL32(40, 40, 45, 255);
    else if (held) top = IM_COL32(112, 12, 18, 255), bottom = IM_COL32(64, 6, 10, 255);
    else if (hover) top = IM_COL32(205, 30, 38, 255), bottom = IM_COL32(118, 12, 18, 255);
    const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + size.x, pos.y}, {pos.x + size.x - sk, pos.y + size.y},
                         {pos.x, pos.y + size.y}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const auto i0 = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimReserve(6, 4);  // a top-to-bottom gradient quad
    dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 1)), dl->PrimWriteIdx(ImDrawIdx(i0 + 2));
    dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 2)), dl->PrimWriteIdx(ImDrawIdx(i0 + 3));
    dl->PrimWriteVtx(q[0], uv, top), dl->PrimWriteVtx(q[1], uv, top);
    dl->PrimWriteVtx(q[2], uv, bottom), dl->PrimWriteVtx(q[3], uv, bottom);
    dl->AddPolyline(q, 4, enabled ? IM_COL32_WHITE : IM_COL32(150, 150, 156, 255), ImDrawFlags_Closed, 3.5f * d);
    const std::string text(label, ImGui::FindRenderedTextEnd(label));
    ImVec2 ts = MenuTextSize(font_px, text.c_str());
    const float room = size.x - 2 * sk - 16 * d;
    if (ts.x > room) font_px *= room / ts.x, ts = MenuTextSize(font_px, text.c_str());
    MenuText(dl, ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + (size.y - ts.y) / 2), font_px,
             enabled ? IM_COL32_WHITE : IM_COL32(170, 170, 176, 255), text.c_str());
    return pressed;
  }

  // The logo (resource 101), scaled once per drawn width (slot 0 the Play screen, 1 About, 2 the band). Returns the drawn size;
  // (0, 0) when the exe has no logo.
  ImVec2 DrawLogo(ImDrawList* dl, ImVec2 pos, float width, int slot) {
    if (!logo_tried_) {
      logo_tried_ = true;
      const HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
      const HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
      const auto* data = static_cast<const stbi_uc*>(g ? LockResource(g) : nullptr);
      int n = 0;
      if (data)
        if (stbi_uc* px = stbi_load_from_memory(data, int(SizeofResource(nullptr, r)), &logo_w_, &logo_h_, &n, 4)) {
          logo_px_.assign(px, px + size_t(logo_w_) * size_t(logo_h_) * 4);
          stbi_image_free(px);
        }
    }
    if (logo_px_.empty()) return ImVec2(0, 0);
    const int w = std::clamp(int(width) / 8 * 8, 16, logo_w_);  // steps of 8 px while the window is resized
    const int h = std::max(1, int(std::lround(double(logo_h_) * w / logo_w_)));
    auto& srv = logo_srv_[slot];
    if (!srv || logo_srv_w_[slot] != w) {
      ReleaseLater(srv);
      srv = MakeTexture(w == logo_w_ ? logo_px_ : theme::Resample(logo_px_.data(), logo_w_, logo_h_, w, h), w, h);
      logo_srv_w_[slot] = w;
    }
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), pos, ImVec2(pos.x + float(w), pos.y + float(h)));
    return ImVec2(float(w), float(h));
  }

  // Menu text: the game's own face when the game files have it (LoadMenuFont), else Arial Black. px = the line
  // height (the game face's 40-pixel cell, scaled).
  static uint32_t NextCodepoint(const char*& p) {
    const auto c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3Fu >> extra);
    for (int i = 0; i < extra && (static_cast<unsigned char>(*p) & 0xC0) == 0x80; ++i) cp = cp << 6 | (*p++ & 0x3F);
    return cp;
  }
  ImVec2 MenuTextSize(float px, const char* text) {
    if (gfont_srv_) {
      const float k = px / gfont_.cell;
      float w = 0;
      for (const char* p = text; *p;) {
        const auto it = gfont_.glyphs.find(NextCodepoint(p));
        w += (it != gfont_.glyphs.end() ? it->second.w : gfont_.cell * 0.33f) * k;
      }
      return ImVec2(w, px);
    }
    ImGui::PushFont(g_font_menu, px / Dpi());  // ImGui scales font sizes by the DPI itself
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return sz;
  }
  void MenuTextRaw(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text) {
    if (!gfont_srv_) {
      ImGui::PushFont(g_font_menu, px / Dpi());
      dl->AddText(pos, col, text);
      ImGui::PopFont();
      return;
    }
    const float k = px / gfont_.cell, iw = 1.f / float(gfont_.sheet_w), ih = 1.f / float(gfont_.sheet_h);
    const ImTextureRef tex(reinterpret_cast<ImTextureID>(gfont_srv_));
    float x = std::floor(pos.x);
    const float y = std::floor(pos.y);
    for (const char* p = text; *p;) {
      const auto it = gfont_.glyphs.find(NextCodepoint(p));
      if (it == gfont_.glyphs.end()) {
        x += gfont_.cell * 0.33f * k;
        continue;
      }
      const GameFont::Glyph& g = it->second;
      dl->AddImage(tex, ImVec2(x, y), ImVec2(x + g.w * k, y + g.h * k), ImVec2(g.x * iw, g.y * ih),
                   ImVec2((g.x + g.w) * iw, (g.y + g.h) * ih), col);
      x += g.w * k;
    }
  }
  // With the game's drop shadow.
  void MenuText(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text) {
    const float o = std::max(1.f, px * 0.06f);
    MenuTextRaw(dl, ImVec2(pos.x + o, pos.y + o), px, IM_COL32(0, 0, 0, 190), text);
    MenuTextRaw(dl, pos, px, col, text);
  }

  // "version 0.1.4" for the big screens (the footer keeps the full string).
  static std::string ShortVersion() {
    std::string v = Version();
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    return "version " + v.substr(0, v.find_first_of("-+"));
  }

  // A page title in the menu font, with "< BACK" before it on the Play sub-screens.
  bool TitleWithBack(const char* title) {
    const float d = Dpi();
    bool back = false;
    ImGui::PushFont(g_font_menu, 15);
    back = ImGui::Button("< BACK");
    ImGui::PopFont();
    ImGui::SameLine(0, 14 * d);
    Heading(title, 1.5f);
    return back;
  }

  // ---------------- Play
  int play_view_ = 0;  // Session 31: 0 the home screen (logo, PLAY GAME, QUICK MATCH), 1 Quick Match, 2 Advanced

  void PlayPage() {
    if (play_view_ == 1) {
      if (TitleWithBack("QUICK MATCH")) play_view_ = 0;
      ImGui::Spacing();
      QuickMatch();
      return;
    }
    if (play_view_ == 3) {
      if (TitleWithBack("30 YEARS OF WRESTLEMANIA")) play_view_ = 0;
      Wm30Page();
      return;
    }
    if (play_view_ == 2) {
      if (TitleWithBack("ADVANCED")) play_view_ = 0;
      ImGui::TextDisabled("Replays, re-shoots, seeds, director scripts and the exact command line.");
      ImGui::Spacing();
      AdvancedPlay();
      return;
    }
    PlayHome();
  }

  // The home screen, as in Jarrod's sketch: the logo, "NATIVE PC RECOMPILATION / version", two big slanted buttons.
  void PlayHome() {
    const float d = Dpi();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    const float want_w = std::min(avail.x * 0.8f, 640 * d);
    const float logo_y = o.y + std::max(6 * d, avail.y * 0.05f);
    ImVec2 logo = DrawLogo(dl, ImVec2(o.x + (avail.x - want_w) / 2, logo_y), want_w, 0);
    float y = logo_y + logo.y;
    const float sub_px = 22 * d;
    if (logo.x > 0) {
      // Under "2K14", tucked into the logo's empty lower right (the signature runs under the W on the left).
      const float cx = o.x + (avail.x - logo.x) / 2 + logo.x * 0.655f;
      y = logo_y + logo.y * 0.80f;
      const char* sub = "NATIVE PC RECOMPILATION";
      ImVec2 ts = MenuTextSize(sub_px, sub);
      MenuText(dl, ImVec2(cx - ts.x / 2, y), sub_px, IM_COL32_WHITE, sub);
      y += ts.y;
      const std::string ver = ShortVersion();
      ts = MenuTextSize(sub_px * 0.9f, ver.c_str());
      MenuText(dl, ImVec2(cx - ts.x / 2, y), sub_px * 0.9f, IM_COL32_WHITE, ver.c_str());
      y += ts.y;
    } else {
      MenuText(dl, ImVec2(o.x, y), 48 * d, IM_COL32_WHITE, "WWE 2K14");
      y += 60 * d;
    }

    y = std::max(y + 34 * d, logo_y + logo.y + 16 * d);
    if (!gd_.ok) {
      ImGui::SetCursorScreenPos(ImVec2(o.x + avail.x * 0.18f, y));
      ImGui::TextColored(kBad, "Your game files aren't set up yet.");
      ImGui::SameLine();
      if (ImGui::SmallButton("Go to Settings")) GoPage(2);
      y += ImGui::GetFrameHeightWithSpacing();
    }
    const float gap = 44 * d, bw = std::min(330 * d, (avail.x - gap - 20 * d) / 2), bh = 116 * d;
    const float bx = o.x + (avail.x - 2 * bw - gap) / 2;
    if (SlantButton("PLAY GAME", ImVec2(bx, y), ImVec2(bw, bh), gd_.ok && !game_proc_, 36 * d)) LaunchPlain();
    if (SlantButton("QUICK MATCH", ImVec2(bx + bw + gap, y), ImVec2(bw, bh), true, 36 * d)) play_view_ = 1;
    const float buttons_bottom = y + bh;  // Session 34: the 30 YEARS button continues these two's outer slanted edges
    y += bh + 8 * d;
    auto caption = [&](float x, const char* text) {
      const ImVec2 ts = ImGui::CalcTextSize(text);
      ImGui::SetCursorScreenPos(ImVec2(x + (bw - ts.x) / 2, y));
      ImGui::TextDisabled("%s", text);
    };
    caption(bx + bw + gap, "pick the match, wrestlers and arena");
    // Session 34: every 30 Years of WrestleMania match, one click away (Play -> 30 YEARS). Its left and right edges
    // lie on the lines of PLAY GAME's left and QUICK MATCH's right edge (SlantButton slants 0.22 x its height, the
    // same slope at any height): shifted left by the slant over the gap below them, narrower by the slant they have
    // and it hasn't (Jarrod: "it otherwise looks uncentered").
    y += ImGui::GetTextLineHeightWithSpacing() + 12 * d;
    const float wh = 64 * d, wx = bx - 0.22f * (y + wh - buttons_bottom), ww = 2 * bw + gap - 0.22f * (bh - wh);
    if (SlantButton("30 YEARS OF WRESTLEMANIA", ImVec2(wx, y), ImVec2(ww, wh), true, 30 * d)) play_view_ = 3;
    y += wh + 8 * d;
    {
      const char* text = "any of the mode's 46 matches, straight from the main menu";
      const ImVec2 ts = ImGui::CalcTextSize(text);
      ImGui::SetCursorScreenPos(ImVec2(wx + (ww - 0.22f * wh - ts.x) / 2, y));
      ImGui::TextDisabled("%s", text);
    }
    if (PerfTestAvailable()) {  // Session 39: a test build's performance test (PerfTestPopup)
      y += ImGui::GetTextLineHeightWithSpacing() + 10 * d;
      ImGui::SetCursorScreenPos(ImVec2(wx + (ww - 0.22f * wh) / 2 - 110 * d, y));
      if (ImGui::Button("PERFORMANCE TEST (test build)", ImVec2(220 * d, 0))) want_perf_popup_ = true;
      y += ImGui::GetFrameHeight();
    }

    // Bottom: what Play uses, the keys, and the Advanced tools.
    const float line = ImGui::GetTextLineHeightWithSpacing();
    float by = o.y + avail.y - line * 2 - ImGui::GetFrameHeight() - 6 * d;
    by = std::max(by, y + line * 2);
    ImGui::SetCursorScreenPos(ImVec2(o.x, by));
    RefreshDlc(30);  // Session 32: "DLC: N packs on"
    const int dlc_on = DlcPacksOn();
    const std::string dlc_text = dlc_on ? ", DLC: " + std::to_string(dlc_on) + (dlc_on == 1 ? " pack on" : " packs on") : "";
    ImGui::TextDisabled("%s%s%s%s.  F8 CineCam (when on in Settings), Alt+F4 quits.",
                        ResolutionName(s).c_str(),
                        s.frame_rate == 2   ? (s.refresh_hz == 120 ? ", 120 fps" : ", uncapped")
                        : s.frame_rate == 0 ? ", classic frame rate"
                        : s.frame_rate == 3 ? ", 30 fps"  // Session 41 (it said 60)
                                            : ", 60 fps",
                        s.upscale ? ", upscaling on" : "", dlc_text.c_str());
    if (ImGui::Button("Advanced: replays, re-shoots, seeds, director...")) play_view_ = 2;
  }

  static constexpr const char* kResolutionNames[] = {"720p", "1440p", "4K"};
  // Session 54: the size the game draws at (360p ... 4K): a draw height only with NATIVE and no upscaler.
  static bool DrawHeightApplies(const Settings& t) { return t.renderer == 2 && t.upscale == 0 && !t.original_look; }
  static std::string ResolutionName(const Settings& t) {
    if (t.original_look) return kResolutionNames[0];
    if (DrawHeightApplies(t) && t.draw_height) return std::to_string(t.draw_height) + "p";
    return kResolutionNames[std::clamp(t.resolution, 0, 2)];
  }

  // The match types a Quick Match can build, laid out like the game's PLAY menu (its order, from the Session 30 menu
  // crawl; db\ids\match_types.json). type = the game's match type id (docs\systems\match_setup.md); group = the
  // game's submenu, if any; sides = one team digit per wrestler slot, as the game's apply sets them (slot +13), so its
  // length is the wrestler count. Checked: Session 27 (0, 3, 37, 39, 49), Session 30 (13, 14, 33, 55, 60, 81), Session
  // 31 the other 48 (runs 2357-2404: each loaded and ran 10 s of match clock; the sides from the log; the pictures
  // show the cage, the cell, the chamber wall, ladders, backstage where expected). Not here: the Royal Rumble (its
  // entry order comes from the menus: My saved setups), Special Referee, Title Match, Tournaments.
  struct QuickType {
    int type;
    const char* cat;
    const char* group;
    const char* name;
    const char* sides;
  };
  static constexpr const char* kQuickCats[] = {"ONE ON ONE", "TWO ON TWO", "TRIPLE THREAT", "FATAL 4-WAY",
                                               "6-MAN",      "HANDICAP",   "SPECIALTY"};
  static constexpr QuickType kQuickTypes[] = {
      {0, "ONE ON ONE", "", "NORMAL", "01"},
      {77, "ONE ON ONE", "", "EXTREME RULES", "01"},
      {49, "ONE ON ONE", "", "HELL IN A CELL", "01"},
      {33, "ONE ON ONE", "", "I QUIT", "01"},
      {81, "ONE ON ONE", "", "INFERNO", "01"},
      {54, "ONE ON ONE", "", "IRON MAN", "01"},
      {55, "ONE ON ONE", "", "LADDER", "01"},
      {59, "ONE ON ONE", "", "LAST MAN STANDING", "01"},
      {60, "ONE ON ONE", "", "STEEL CAGE", "01"},
      {67, "ONE ON ONE", "", "SUBMISSION", "01"},
      {68, "ONE ON ONE", "", "TABLE", "01"},
      {72, "ONE ON ONE", "", "TLC", "01"},
      {3, "TWO ON TWO", "", "TAG TEAM", "0101"},
      {9, "TWO ON TWO", "ELIMINATION TAG", "TAG TEAM", "0101"},
      {10, "TWO ON TWO", "ELIMINATION TAG", "TORNADO TAG", "0101"},
      {12, "TWO ON TWO", "ELIMINATION TAG", "TORNADO TAG TABLE", "0101"},
      {7, "TWO ON TWO", "", "MIXED GENDER TAG", "0101"},
      {4, "TWO ON TWO", "TORNADO TAG", "TAG TEAM", "0101"},
      {78, "TWO ON TWO", "TORNADO TAG", "EXTREME RULES", "0101"},
      {50, "TWO ON TWO", "TORNADO TAG", "HELL IN A CELL", "0101"},
      {56, "TWO ON TWO", "TORNADO TAG", "LADDER", "0101"},
      {61, "TWO ON TWO", "TORNADO TAG", "STEEL CAGE", "0101"},
      {69, "TWO ON TWO", "TORNADO TAG", "TABLE", "0101"},
      {73, "TWO ON TWO", "TORNADO TAG", "TLC", "0101"},
      {13, "TRIPLE THREAT", "", "NORMAL", "012"},
      {79, "TRIPLE THREAT", "", "EXTREME RULES", "012"},
      {51, "TRIPLE THREAT", "", "HELL IN A CELL", "012"},
      {57, "TRIPLE THREAT", "", "LADDER", "012"},
      {62, "TRIPLE THREAT", "", "STEEL CAGE", "012"},
      {70, "TRIPLE THREAT", "", "TABLE", "012"},
      {74, "TRIPLE THREAT", "", "TLC", "012"},
      {14, "FATAL 4-WAY", "", "NORMAL", "0123"},
      {34, "FATAL 4-WAY", "", "BATTLE ROYAL", "0123"},
      {80, "FATAL 4-WAY", "", "EXTREME RULES", "0123"},
      {52, "FATAL 4-WAY", "", "HELL IN A CELL", "0123"},
      {58, "FATAL 4-WAY", "", "LADDER", "0123"},
      {63, "FATAL 4-WAY", "", "STEEL CAGE", "0123"},
      {71, "FATAL 4-WAY", "", "TABLE", "0123"},
      {75, "FATAL 4-WAY", "", "TLC", "0123"},
      {35, "6-MAN", "", "BATTLE ROYAL", "012345"},
      {39, "6-MAN", "ELIMINATION CHAMBER", "NORMAL", "012345"},
      {40, "6-MAN", "ELIMINATION CHAMBER", "TORNADO TAG", "010101"},
      {11, "6-MAN", "", "ELIMINATION TAG", "010101"},
      {53, "6-MAN", "", "HELL IN A CELL", "012345"},
      {76, "6-MAN", "", "LADDER", "012345"},
      {8, "6-MAN", "", "TAG TEAM", "010101"},
      {37, "6-MAN", "", "6-PACK CHALLENGE", "012345"},  // not in PLAY (Session 27's six-man type)
      {16, "HANDICAP", "ONE ON TWO", "TAG TEAM", "011"},
      {15, "HANDICAP", "ONE ON TWO", "TORNADO TAG", "011"},
      {18, "HANDICAP", "ONE ON THREE", "TAG TEAM", "0111"},
      {17, "HANDICAP", "ONE ON THREE", "TORNADO TAG", "0111"},
      {19, "HANDICAP", "", "TWO ON THREE", "01011"},
      {82, "HANDICAP", "", "GAUNTLET", "0111"},
      {26, "SPECIALTY", "BACKSTAGE BRAWL", "ONE ON ONE", "01"},
      {29, "SPECIALTY", "BACKSTAGE BRAWL", "TWO ON TWO", "0101"},
      {27, "SPECIALTY", "BACKSTAGE BRAWL", "ONE ON TWO", "011"},
      {28, "SPECIALTY", "BACKSTAGE BRAWL", "ONE ON THREE", "0111"},
      {30, "SPECIALTY", "BACKSTAGE BRAWL", "TRIPLE THREAT", "012"},
      {31, "SPECIALTY", "BACKSTAGE BRAWL", "FATAL 4-WAY", "0123"},
  };
  static int QuickIndex(int type) {
    if (type == 1) type = 13;  // Session 27's triple threat / fatal 4-way ids (not in PLAY): the menu's own ones
    if (type == 2) type = 14;
    for (int i = 0; i < int(std::size(kQuickTypes)); ++i)
      if (kQuickTypes[i].type == type) return i;
    return 0;
  }
  // "TEAM 1" / "TEAM 2" for team matches, "WRESTLER N" when everyone is on their own.
  static std::string SlotLabel(const QuickType& t, size_t slot) {
    const std::string sides = t.sides;
    const bool solo = std::all_of(sides.begin(), sides.end(),
                                  [&](char c) { return std::count(sides.begin(), sides.end(), c) == 1; });
    return solo ? "Wrestler " + std::to_string(slot + 1) : "Team " + std::string(1, char(sides[slot] + 1));
  }

  static std::vector<int> ParseIds(const std::string& text) {
    std::vector<int> out;
    std::stringstream ss(text);
    for (std::string t; std::getline(ss, t, ',');)
      if (!t.empty()) out.push_back(std::atoi(t.c_str()));
    return out;
  }

  std::vector<const Character*> Wrestlers() const {
    std::vector<const Character*> w;
    for (const auto& c : chars_)
      if (c.kind == "wrestler") w.push_back(&c);
    for (const auto& c : dlc_chars_)  // Session 32: the DLC wrestlers whose pack is on
      if (DlcWrestlerOn(c)) w.push_back(&c);
    return w;
  }

  // One wrestler picker: a combo that filters as you type.
  bool WrestlerCombo(const char* id, int& value) {
    bool changed = false;
    const std::string label = value >= 0 ? CharName(value) : std::string("(pick)");
    if (ImGui::BeginCombo(id, label.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(), pick_search_.clear();
      ImGui::InputTextWithHint("##search", "type to search", &pick_search_);
      const std::string q = Lower(pick_search_);
      for (const Character* c : Wrestlers()) {
        if (!q.empty() && Lower(c->name).find(q) == std::string::npos) continue;
        if (ImGui::Selectable(c->name.c_str(), c->id == value)) value = c->id, changed = true;
      }
      ImGui::EndCombo();
    }
    return changed;
  }

  // A list entry in the style of the game's second menu column: grey, white with the red arrow when selected.
  bool ListItem(const char* label, bool selected, float px, float width) {
    const float d = Dpi(), h = px * 1.2f, arrow = px * 0.5f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, h));
    const bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
      const float cy = pos.y + h / 2;
      dl->AddTriangleFilled(ImVec2(pos.x, cy - arrow / 2), ImVec2(pos.x + arrow * 0.9f, cy),
                            ImVec2(pos.x, cy + arrow / 2), kMenuRed);
    }
    const ImU32 col = selected ? IM_COL32_WHITE : hover ? IM_COL32(205, 206, 211, 255) : IM_COL32(141, 142, 147, 255);
    MenuText(dl, ImVec2(pos.x + arrow + 8 * d, pos.y + (h - px) / 2), px, col, label);
    return pressed;
  }

  // A game-style option row, like the in-game GRAPHICS menu (Session 28): LABEL on the left, "< VALUE >" on the
  // right. The arrows step through the values; a click on the label or value steps forward. A faint slanted bar
  // marks the hovered row. `note` (optional) is a grey line under the row. Returns true when the value changed.
  bool OptionRow(const char* label, int& value, const char* const* names, int count, bool enabled = true,
                 const char* note = nullptr, float width = 0) {
    const float d = Dpi(), px = 20 * d, h = 38 * d, w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float val_w = std::min(300 * d, w * 0.5f), arrow = 9 * d;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(label);
    ImGui::BeginDisabled(!enabled);
    const bool click = ImGui::InvisibleButton("##row", ImVec2(w, h));
    ImGui::EndDisabled();
    const bool hover = enabled && ImGui::IsItemHovered();
    ImGui::PopID();
    value = std::clamp(value, 0, count - 1);
    const int before = value;
    if (click) {
      const float mx = ImGui::GetIO().MousePos.x, vx = pos.x + w - val_w;
      value = mx >= vx && mx < vx + arrow * 3 ? (value + count - 1) % count : (value + 1) % count;
    }
    if (hover) {
      const float sk = 10 * d;
      const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + w, pos.y}, {pos.x + w - sk, pos.y + h}, {pos.x, pos.y + h}};
      dl->AddConvexPolyFilled(q, 4, IM_COL32(0, 0, 0, 120));
    }
    const ImU32 text = enabled ? IM_COL32_WHITE : IM_COL32(110, 111, 116, 255);
    MenuText(dl, ImVec2(pos.x + 14 * d, pos.y + (h - px) / 2), px, text, label);
    // The value, centred between the two arrows.
    const float vx = pos.x + w - val_w, cy = pos.y + h / 2;
    const ImU32 arrow_col = enabled ? kMenuRed : IM_COL32(90, 30, 32, 255);
    dl->AddTriangleFilled(ImVec2(vx + arrow, cy - arrow), ImVec2(vx + arrow, cy + arrow), ImVec2(vx, cy), arrow_col);
    dl->AddTriangleFilled(ImVec2(vx + val_w - arrow - 6 * d, cy - arrow), ImVec2(vx + val_w - 6 * d, cy),
                          ImVec2(vx + val_w - arrow - 6 * d, cy + arrow), arrow_col);
    const char* shown = names[value];
    float vpx = px;
    ImVec2 ts = MenuTextSize(vpx, shown);
    const float room = val_w - 4 * arrow - 12 * d;
    if (ts.x > room) vpx *= room / ts.x, ts = MenuTextSize(vpx, shown);
    MenuText(dl, ImVec2(vx + (val_w - 6 * d - ts.x) / 2, cy - ts.y / 2), vpx, text, shown);
    if (note && *note) {
      ImGui::SetCursorScreenPos(ImVec2(pos.x + 14 * d, ImGui::GetCursorScreenPos().y - 4 * d));
      ImGui::PushTextWrapPos(pos.x + w - ImGui::GetWindowPos().x);
      ImGui::TextDisabled("%s", note);
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0, 2 * d));
    }
    return value != before;
  }
  bool OptionToggle(const char* label, bool& on, bool enabled = true, const char* note = nullptr, float width = 0) {
    static constexpr const char* kOffOn[] = {"OFF", "ON"};
    int v = on ? 1 : 0;
    const bool changed = OptionRow(label, v, kOffOn, 2, enabled, note, width);
    on = v == 1;
    return changed;
  }
  // A red section heading inside a page (TEAM 1, ARENA, ...).
  void SubHeading(const char* text) {
    const float d = Dpi();
    MenuTextRaw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 15 * d, kMenuRed, text);
    ImGui::Dummy(ImVec2(0, 19 * d));
  }

  // Session 31: the game's PLAY menu as two columns (category, then its matches with the game's submenus as
  // headings), the setup beside them (below them in a narrow window).
  void BuildMatch() {
    const float d = Dpi(), px = 21 * d, cat_w = 205 * d, type_w = 270 * d;
    int ti = QuickIndex(s.quick_type);
    s.quick_type = kQuickTypes[ti].type;  // review fix: a stored type not in the list launches as the one shown
    ImGui::BeginGroup();
    for (const char* cat : kQuickCats) {
      const bool on = std::strcmp(cat, kQuickTypes[ti].cat) == 0;
      if (ListItem(cat, on, px, cat_w) && !on)  // like the game: the cursor lands on the category's first match
        for (int i = 0; i < int(std::size(kQuickTypes)); ++i)
          if (std::strcmp(kQuickTypes[i].cat, cat) == 0) {
            ti = i, s.quick_type = kQuickTypes[i].type;
            break;
          }
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, 6 * d);
    ImGui::BeginGroup();
    const char* group = "";
    for (int i = 0; i < int(std::size(kQuickTypes)); ++i) {
      const QuickType& t = kQuickTypes[i];
      if (std::strcmp(t.cat, kQuickTypes[ti].cat) != 0) continue;
      if (*t.group && std::strcmp(group, t.group) != 0) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        MenuTextRaw(ImGui::GetWindowDrawList(), ImVec2(at.x, at.y + 4 * d), 15 * d, kMenuRed, t.group);
        ImGui::Dummy(ImVec2(type_w, 21 * d));
      }
      group = t.group;
      const float indent = *t.group ? 14 * d : 0;
      ImGui::PushID(t.type);
      if (indent > 0) ImGui::Indent(indent);
      if (ListItem(t.name, i == ti, px * 0.86f, type_w - indent)) ti = i, s.quick_type = t.type;
      if (indent > 0) ImGui::Unindent(indent);
      ImGui::PopID();
    }
    if (std::strcmp(kQuickTypes[ti].cat, "SPECIALTY") == 0) {
      ImGui::Dummy(ImVec2(0, 6 * d));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + type_w);
      ImGui::TextDisabled("Royal Rumble: set it up once in the game's menus with My saved setups (its entry order "
                          "comes from the menus).");
      ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    const float left = ImGui::GetContentRegionAvail().x - cat_w - type_w - 6 * d;
    if (left >= 400 * d) ImGui::SameLine(0, 24 * d);
    else ImGui::Dummy(ImVec2(0, 10 * d));
    ImGui::BeginGroup();
    MatchSetup(kQuickTypes[ti], std::min(left >= 400 * d ? left - 24 * d : ImGui::GetContentRegionAvail().x, 520 * d));
    ImGui::EndGroup();
  }

  // The chosen match: its wrestlers (grouped by team), arena, options, START MATCH.
  void MatchSetup(const QuickType& qt, float width) {
    const float d = Dpi(), w = width - 96 * d;  // room for a full-height Random button
    {
      const std::string path = std::string(qt.cat) + (*qt.group ? std::string("  >  ") + qt.group : "") + "  >  " + qt.name;
      float px = 19 * d;
      if (const float tw = MenuTextSize(px, path.c_str()).x; tw > width) px *= width / tw;  // long ones: smaller
      MenuText(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), px, IM_COL32_WHITE, path.c_str());
      ImGui::Dummy(ImVec2(width, 26 * d));
    }
    const std::string sides = qt.sides;
    std::vector<int> ids = ParseIds(s.quick_ids);
    ids.resize(sides.size(), -1);
    const auto pool = Wrestlers();
    auto random_id = [&](const std::vector<int>& taken) {
      for (int tries = 0; tries < 200 && !pool.empty(); ++tries) {
        const int id = pool[size_t(rng_() % pool.size())]->id;
        if (std::find(taken.begin(), taken.end(), id) == taken.end()) return id;
      }
      return pool.empty() ? -1 : pool.front()->id;
    };
    // The slots in team order (the game decides the sides by slot: qt.sides), each team under its own heading.
    const bool solo = SlotLabel(qt, 0).rfind("Wrestler", 0) == 0;
    for (char team = '0'; team <= '9'; ++team) {
      if (sides.find(team) == std::string::npos) continue;
      if (!solo) {
        const std::string head = "TEAM " + std::string(1, char(team + 1));
        MenuTextRaw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 15 * d, kMenuRed, head.c_str());
        ImGui::Dummy(ImVec2(0, 17 * d));
      }
      for (size_t i = 0; i < ids.size(); ++i) {
        if (sides[i] != team) continue;
        ImGui::PushID(int(i));
        ImGui::SetNextItemWidth(w);
        WrestlerCombo(("##w" + std::to_string(i)).c_str(), ids[i]);
        ImGui::SameLine();
        if (ImGui::Button("Random")) {  // full height, like the pickers (Jarrod)
          std::vector<int> others = ids;
          others.erase(others.begin() + std::ptrdiff_t(i));
          ids[i] = random_id(others);
        }
        ImGui::PopID();
      }
    }
    ImGui::Dummy(ImVec2(0, 4 * d));
    std::string arena_label = "The match type's own arena";
    for (const auto& a : arenas_)
      if (a.id == s.quick_arena) arena_label = a.name;
    MenuTextRaw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 15 * d, kMenuRed, "ARENA");
    ImGui::Dummy(ImVec2(0, 17 * d));
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##arena", arena_label.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::Selectable("The match type's own arena", s.quick_arena < 0)) s.quick_arena = -1;
      for (const auto& a : arenas_)
        if (ImGui::Selectable((a.name + "##" + std::to_string(a.id)).c_str(), a.id == s.quick_arena)) s.quick_arena = a.id;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Random##arena") && !arenas_.empty()) s.quick_arena = arenas_[size_t(rng_() % arenas_.size())].id;
    if (ImGui::Button("Random everything")) {
      std::vector<int> taken;
      for (auto& id : ids) id = random_id(taken), taken.push_back(id);
      if (!arenas_.empty()) s.quick_arena = arenas_[size_t(rng_() % arenas_.size())].id;
    }
    ImGui::Checkbox("Skip entrances", &s.skip_entrances);
    ImGui::SameLine();
    ImGui::Checkbox("CPU vs CPU (watch)", &s.quick_cpu_only);
    if (!s.quick_cpu_only)
      ImGui::TextDisabled("You play %s with controller 1.", solo ? "wrestler 1" : "Team 1's first wrestler");
    std::string text;
    for (int id : ids) text += (text.empty() ? "" : ",") + std::to_string(id);
    s.quick_ids = text;
    const bool complete = std::find(ids.begin(), ids.end(), -1) == ids.end();
    ImGui::Dummy(ImVec2(0, 6 * d));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (SlantButton("START MATCH", at, ImVec2(250 * d, 64 * d), complete && gd_.ok && !game_proc_, 28 * d))
      LaunchQuickMatch();
    if (!complete) ImGui::TextDisabled("Pick every wrestler (or press Random).");
  }

  void QuickMatch() {
    const float dpi = Dpi(), label_w = 170 * dpi;
    RefreshDlc(30);  // Session 32: the DLC wrestlers for the pickers
    ImGui::RadioButton("Build a match", &s.quick_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("My saved setups", &s.quick_mode, 1);
    if (s.quick_mode == 0) {
      BuildMatch();
      return;
    }
    std::vector<std::string> names;
    for (const auto& sc : scenarios_) names.push_back(sc.name);
    if (names.empty()) {
      Note("No saved matches yet. \"Save a match setup\" starts the game: set a match up in the menus as usual "
           "(wrestlers, arena, match type). When you confirm it, it is saved here, and next time it starts in one "
           "click, straight from the main menu.");
    } else {
      ImGui::SetNextItemWidth(label_w * 2);
      Combo("Match", s.scenario, names, "(pick a saved match)");
      for (const auto& sc : scenarios_) {
        if (sc.name != s.scenario) continue;
        std::string who;
        for (int id : sc.ids) who += (who.empty() ? "" : ", ") + CharName(id);
        ImGui::TextDisabled("%s%s%s", who.c_str(), sc.description.empty() ? "" : "  -  ", sc.description.c_str());
      }
      ImGui::BeginDisabled(s.scenario.empty());
      ImGui::Checkbox("Other arena", &s.arena_override);
      ImGui::SameLine();
      ImGui::BeginDisabled(!s.arena_override);
      ImGui::SetNextItemWidth(label_w * 2);
      std::string arena_label = "#" + std::to_string(s.arena);
      for (const auto& a : arenas_)
        if (a.id == s.arena) arena_label = a.name + " (" + std::to_string(a.id) + ")";
      if (ImGui::BeginCombo("##qarena", arena_label.c_str())) {
        for (const auto& a : arenas_)
          if (ImGui::Selectable((a.name + " (" + std::to_string(a.id) + ")").c_str(), a.id == s.arena)) s.arena = a.id;
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
      ImGui::SetNextItemWidth(label_w * 2);
      ImGui::InputTextWithHint("Wrestlers", "empty = the saved match's (ids, comma separated)", &s.participants);
      ImGui::SameLine();
      if (ImGui::SmallButton("Add...")) ImGui::OpenPopup("pick_char");
      CharPicker();
      ImGui::Checkbox("Skip entrances", &s.skip_entrances);
      ImGui::EndDisabled();
      ImGui::BeginDisabled(s.scenario.empty() || !gd_.ok || game_proc_ != nullptr);
      if (BigButton("Start match", ImVec2(200 * dpi, 40 * dpi), ImVec4(0.5f, 0.12f, 0.12f, 1))) LaunchQuickMatch();
      ImGui::EndDisabled();
      ImGui::SameLine();
    }
    ImGui::BeginDisabled(!gd_.ok || game_proc_ != nullptr);
    if (ImGui::Button("Save a match setup...", ImVec2(0, 40 * dpi))) ImGui::OpenPopup("capture");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("capture")) {
      ImGui::TextUnformatted("Name for the match (letters, numbers, _):");
      ImGui::SetNextItemWidth(260 * dpi);
      ImGui::InputText("##capname", &capture_name_);
      std::string clean;
      for (char c : capture_name_)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') clean += c;
      ImGui::TextDisabled("The game starts. Set the match up in the menus and confirm it: it is saved as \"%s\".",
                          clean.empty() ? "my_match" : clean.c_str());
      if (ImGui::Button("Start the game")) {
        LaunchCapture(clean.empty() ? "my_match" : clean);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  void CharPicker() {
    if (ImGui::BeginPopup("pick_char")) {
      ImGui::SetNextItemWidth(260);
      ImGui::InputTextWithHint("##pq", "search", &pick_search_);
      ImGui::BeginChild("pk", ImVec2(300, 300));
      const std::string q = Lower(pick_search_);
      for (const auto& c : chars_) {
        if (!q.empty() && Lower(c.name).find(q) == std::string::npos) continue;
        if (ImGui::Selectable((c.name + "  (" + std::to_string(c.id) + ")").c_str())) {
          s.participants += (s.participants.empty() ? "" : ",") + std::to_string(c.id);
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::EndChild();
      ImGui::EndPopup();
    }
  }

  // The game with the player's settings: no scenario, the Play preset, not through the test harness.
  Settings PlainSettings() const {
    Settings p = s;
    p.scenario.clear(), p.preset = kPlay, p.harness = false;
    return p;
  }
  void LaunchPlain() { StartGame(BuildCommand(PlainSettings(), paths_), "Started the game"); }

  // ---------------- Play -> 30 Years of WrestleMania (Session 34; src\launcher\wm30_list.h, src\wm30_jump.h)
  wwe2k14::wm30list::Catalogue wm30_;
  int wm30_era_ = 0, wm30_sel_ = -1;

  void Wm30Page() {
    const float d = Dpi();
    if (!wm30_.loaded && gd_.ok) wm30_ = wwe2k14::wm30list::Load(GameFilesAt(paths_.game_data));
    if (!gd_.ok) {
      ImGui::TextColored(kBad, "Your game files aren't set up yet.");
      if (ImGui::SmallButton("Go to Settings")) GoPage(2);
      return;
    }
    if (!wm30_.ok) {
      ImGui::TextColored(kBad, "Couldn't read the 30 Years of WrestleMania matches from your game files.");
      ImGui::TextDisabled("%s", wm30_.error.c_str());
      if (ImGui::SmallButton("Try again")) wm30_ = {};
      return;
    }
    ImGui::TextDisabled("The game starts at its main menu and goes to the match by itself (its own videos, story card, "
                        "objectives). Locked matches work too.");
    ImGui::Spacing();
    std::string upper[5];
    const char* names[5];
    for (int e = 0; e < 5; ++e) {
      upper[e] = wm30_.era_names[e];
      for (char& ch : upper[e]) ch = char(std::toupper(static_cast<unsigned char>(ch)));
      // Short tab labels so all five fit: HULKAMANIA, NEW GENERATION, ATTITUDE, RUTHLESS AGGRESSION, UNIVERSE.
      if (upper[e].rfind("THE ", 0) == 0) upper[e].erase(0, 4);
      for (const char* tail : {" RUNS WILD", " ERA"})
        if (upper[e].size() > std::strlen(tail) && upper[e].ends_with(tail)) upper[e].resize(upper[e].size() - std::strlen(tail));
      names[e] = upper[e].c_str();
    }
    if (GameTabs(names, 5, wm30_era_)) wm30_sel_ = -1;
    ImGui::Spacing();
    const float list_w = std::min(ImGui::GetContentRegionAvail().x * 0.48f, 520 * d);
    const float h = ImGui::GetContentRegionAvail().y - 90 * d;
    ImGui::BeginChild("wm30_list", ImVec2(list_w, std::max(h, 120 * d)), ImGuiChildFlags_Borders);
    for (const auto& m : wm30_.matches) {
      if (m.era != wm30_era_) continue;
      if (wm30_sel_ < 0) wm30_sel_ = m.index;
      const std::string label = m.title + "##" + std::to_string(m.index);
      if (ImGui::Selectable(label.c_str(), wm30_sel_ == m.index)) wm30_sel_ = m.index;
      ImGui::TextDisabled("   %s, %s", m.show.c_str(), m.date.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("wm30_detail", ImVec2(0, std::max(h, 120 * d)), ImGuiChildFlags_Borders);
    const wwe2k14::wm30list::Match* sel = nullptr;
    for (const auto& m : wm30_.matches)
      if (m.index == wm30_sel_) sel = &m;
    if (sel) {
      SubHeading(sel->title.c_str());
      ImGui::Text("%s, %s, %s", sel->show.c_str(), sel->venue.c_str(), sel->date.c_str());
      if (!sel->championship.empty()) ImGui::Text("Title match: %s", sel->championship.c_str());
      if (!sel->type_name.empty()) ImGui::Text("Match type: %s", sel->type_name.c_str());
      std::string arena;
      for (const auto& a : arenas_)
        if (a.id == sel->arena) arena = a.name;
      if (!arena.empty()) ImGui::Text("Arena: %s", arena.c_str());
      std::string people;
      for (const auto& p : sel->people) people += (people.empty() ? "" : ", ") + p;
      ImGui::TextWrapped("In the match: %s", people.c_str());
      ImGui::Spacing();
      for (const auto& t : sel->stipulations) ImGui::BulletText("%s", t.c_str());
      if (!sel->historical.empty()) ImGui::TextDisabled("Historical objectives:");
      for (const auto& t : sel->historical) ImGui::BulletText("%s", t.c_str());
    }
    ImGui::EndChild();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (SlantButton("PLAY MATCH", ImVec2(at.x, at.y + 8 * d), ImVec2(250 * d, 64 * d), sel && !game_proc_, 28 * d))
      LaunchWm30(*sel);
  }

  void LaunchWm30(const wwe2k14::wm30list::Match& m) {
    Settings q = PlainSettings();
    q.scenario.clear();
    q.wm30 = m.index;
    StartGame(BuildCommand(q, paths_), "Started 30 Years of WrestleMania: " + m.title);
  }
  void LaunchQuickMatch() {
    Settings q = s;
    q.preset = kPlay, q.harness = false;
    if (s.quick_mode == 0) {  // Session 27 item 8: built from our numbers
      // Session 32: a DLC wrestler needs the pack on (the game has no such character otherwise), and an id the
      // launcher doesn't know at all (a DLC wrestler from a folder that has gone) is not sent either.
      for (int id : ParseIds(s.quick_ids)) {
        if (id < 0) continue;
        const bool on_disc = std::any_of(chars_.begin(), chars_.end(), [&](const Character& c) { return c.id == id; });
        const auto dlc = std::find_if(dlc_chars_.begin(), dlc_chars_.end(), [&](const Character& c) { return c.id == id; });
        if (dlc != dlc_chars_.end() && !DlcWrestlerOn(*dlc)) {
          Status(dlc->name + " needs DLC pack " + std::to_string(dlc->dlc_pack) +
                 ": switch it on in Settings -> Game -> Downloadable content, or pick another wrestler");
          return;
        }
        if (!on_disc && dlc == dlc_chars_.end()) {
          Status("Wrestler #" + std::to_string(id) + " isn't available (a DLC wrestler whose pack is missing?): pick "
                 "another wrestler");
          return;
        }
      }
      q.scenario = "@quick";
      StartGame(BuildCommand(q, paths_), "Started a Quick Match");
      return;
    }
    StartGame(BuildCommand(q, paths_), "Started: " + s.scenario);
  }
  void LaunchCapture(const std::string& name) {
    Settings c = PlainSettings();
    c.extra += (c.extra.empty() ? "" : " ") + std::string("--wwe2k14_scenario_capture=") + name;
    std::error_code ec;
    fs::create_directories(paths_.scenarios, ec);
    StartGame(BuildCommand(c, paths_), "Set up your match in the menus; it is saved as " + name);
  }

  // Starts a command; the game itself is watched for an abnormal exit (PollGame).
  // Session 53: a launcher on another settings file (--settings: a test, a fresh profile) gives the game the same file,
  // so the game's GRAPHICS menu and its FSR note never write the player's own one.
  std::string GameSettingsFlag() const {
    return settings_path == SettingsPath() ? "" : "--wwe2k14_settings_file=" + Quote(PathStr(settings_path));
  }
  // A helper run of the game (Build previews, a logo import): where the game files and the save are, as GameFlags
  // gives them to PLAY GAME, the DLC packs (Session 57, tester bug 2: the save may need them) and this launcher's
  // settings file; each ends with a space.
  std::string HelperPathFlags() const {
    std::string a;
    if (!s.game_data_dir.empty()) a += "--game_data_root=" + Quote(s.game_data_dir) + " ";
    if (!s.user_data_dir.empty()) a += "--user_data_root=" + Quote(s.user_data_dir) + " ";
    for (const auto& x : DlcFlags(s, paths_)) a += x + " ";
    if (const std::string f = GameSettingsFlag(); !f.empty()) a += f + " ";
    return a;
  }
  Command WithSettingsFile(Command c) const {
    const std::string f = GameSettingsFlag();
    if (f.empty() || c.exe != paths_.game_exe.wstring() || c.args.find(L"--wwe2k14_settings_file=") != std::wstring::npos)
      return c;  // (TRY AGAIN restarts last_cmd_, which has it already)
    c.args += (c.args.empty() ? L"" : L" ") + Widen(f), c.display += " " + f;
    return c;
  }

  void StartGame(const Command& cmd_in, const std::string& ok_msg) {
    const Command cmd = WithSettingsFile(cmd_in);
    if (game_proc_ || (cmd.exe == paths_.game_exe.wstring() && GameRunning())) {
      Status("The game is already running");
      return;
    }
    HANDLE h = nullptr;
    const std::string err = StartProcess(cmd, &h);
    if (!err.empty()) {
      Status("Could not start: " + err);
      return;
    }
    if (cmd.exe == paths_.game_exe.wstring()) game_proc_ = h, last_cmd_ = cmd;
    else CloseHandle(h);
    Status(ok_msg);
    if (save_settings) SaveSettings(settings_path, s);
    if (cmd.exe == paths_.game_exe.wstring()) {  // what the in-game GRAPHICS screen may change (ReloadGraphicsSettings)
      launch_graphics_.upscale = s.upscale, launch_graphics_.resolution = s.resolution;
      launch_graphics_.draw_height = s.draw_height;  // Session 54 (review of a tester's report: it was dropped)
      launch_graphics_.frame_rate = s.frame_rate, launch_graphics_.original_look = s.original_look;
      launch_graphics_.texture_filter = s.texture_filter;
      launch_graphics_set_ = true;
    }
  }

  // The game's own GRAPHICS screen (Options) writes the Upscaling / Resolution / Frame rate keys of this same file:
  // take them over when the game closes, or the next Play (and this window's own exit) would put the old values back.
  // Only the keys the game changed (the file differs from what this window wrote when it started the game) are taken:
  // a choice made in this window while the game was running is not undone.
  void ReloadGraphicsSettings() {
    if (!launch_graphics_set_) return;
    wwe2k14::gfxs::Choice c = launch_graphics_;
    const wwe2k14::gfxs::Present p = wwe2k14::gfxs::Parse(wwe2k14::gfxs::ReadFile(settings_path), c);
    if (p.upscale && c.upscale != launch_graphics_.upscale) s.upscale = c.upscale;
    if (p.resolution && c.resolution != launch_graphics_.resolution) s.resolution = c.resolution;
    // Session 54 (a ROG Ally tester: "whatever set from launcher doesn't get saved to game setting graphics or the
    // other way around"): the in-game 360p - 540p / 1080p choice too - before, the launcher's old value was saved over it.
    if ((p.resolution || p.draw_height) && (c.resolution != launch_graphics_.resolution ||
                                            c.draw_height != launch_graphics_.draw_height)) {
      s.resolution = c.resolution, s.draw_height = c.draw_height;
    }
    if (p.frame_rate && c.frame_rate != launch_graphics_.frame_rate) s.frame_rate = c.frame_rate;
    if (p.original_look && c.original_look != launch_graphics_.original_look) s.original_look = c.original_look;
    if (p.texture_filter && c.texture_filter != launch_graphics_.texture_filter) s.texture_filter = c.texture_filter;
    // Session 37: the game turned FSR off after a failure (src\upscale_guard.h): take its note too.
    std::string note;
    if (wwe2k14::gfxs::GetString(wwe2k14::gfxs::ReadFile(settings_path), "fsr_note", note) && !note.empty() &&
        note != s.fsr_note)
      s.fsr_note = note, Status("FSR could not run on this graphics card and was turned off: " + note);
    launch_graphics_set_ = false;
  }
  wwe2k14::gfxs::Choice launch_graphics_;  // the graphics choices as the game was started with them
  bool launch_graphics_set_ = false;

  // Once per frame: the game (or a logo import) finished?
  void PollGame() {
    if (!game_proc_ || WaitForSingleObject(game_proc_, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(game_proc_, &code);
    CloseHandle(game_proc_);
    game_proc_ = nullptr;
    if (importing_) {
      FinishLogoImport(code);
      return;
    }
    ReloadGraphicsSettings();
    last_exit_ = ExitCodeText(code) + " at " + NowStamp("%Y-%m-%d %H:%M");  // Session 58: for Report a problem
    if (perf_step_ >= 0) {  // Session 39: a performance test run (its exit code goes into the test's about.txt)
      if (code) Log("performance test run exited with " + ExitCodeText(code));
      PerfRunEnded(code);
      return;
    }
    if (code == kGameReportedErrorExitCode) {  // the game showed its own message first: no crash dialog
      Status("The game could not start (or stopped on an error it reported). Its log has the details.");
      return;
    }
    if (code != 0) {
      crash_code_ = code;
      want_crash_popup_ = true;
      Log("game exited with " + ExitCodeText(code));
    }
  }

  // Session 31: a game-style modal: a red header band with the title in the menu face (a slanted end and the game's
  // light rim), a dark panel below. Centred, `width` wide, as tall as its content. Pair with ImGui::EndPopup().
  bool BeginGameModal(const char* id, const char* title, float width) {
    const float d = Dpi(), head = 54 * d;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(width, vp->WorkSize.x - 40 * d), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * d, 18 * d));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    const bool open = ImGui::BeginPopupModal(
        id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(2);
    if (!open) return false;
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), sk = 26 * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + head), IM_COL32(176, 20, 28, 255), IM_COL32(150, 14, 22, 255),
                                IM_COL32(96, 8, 13, 255), IM_COL32(110, 10, 15, 255));
    const ImVec2 cut[3] = {{p.x + w - sk * 2, p.y + head}, {p.x + w, p.y}, {p.x + w, p.y + head}};
    dl->AddTriangleFilled(cut[0], cut[1], cut[2], ImGui::GetColorU32(ImGuiCol_PopupBg));  // the slanted end
    dl->AddLine(ImVec2(p.x + w - sk * 2, p.y + head), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 170), 1.6f * d);
    const float px = 26 * d;
    MenuText(dl, ImVec2(p.x + 26 * d, p.y + (head - px) / 2), px, IM_COL32_WHITE, title);
    ImGui::SetCursorPosY(head + 16 * d);
    return true;
  }

  // The modals' buttons: the main one slanted and red, the others plain, all on one row from the left.
  bool ModalMainButton(const char* label, bool enabled = true) {
    const float d = Dpi();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool r = SlantButton(label, at, ImVec2(std::max(200 * d, MenuTextSize(22 * d, label).x + 70 * d), 50 * d),
                               enabled, 22 * d);
    ImGui::SameLine(0, 14 * d);
    modal_button_y_ = ImGui::GetCursorPosY() + (50 * d - ImGui::GetFrameHeight()) / 2;  // centred on the main one
    return r;
  }
  bool ModalButton(const char* label) {
    ImGui::SetCursorPosY(modal_button_y_);
    const bool r = ImGui::Button(label, ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::SameLine(0, 10 * Dpi());
    return r;
  }
  float modal_button_y_ = 0;

  void CrashPopup() {
    if (want_crash_popup_) ImGui::OpenPopup("crash"), want_crash_popup_ = false;
    if (BeginGameModal("crash", "THE GAME CLOSED UNEXPECTEDLY", 640 * Dpi())) {
      ImGui::Text("The game stopped with code %s.", ExitCodeText(crash_code_).c_str());
      ImGui::TextWrapped("Sorry about that. Trying again often works; a report helps us fix it.");
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("TRY AGAIN")) {
        ImGui::CloseCurrentPopup();
        StartGame(last_cmd_, "Started the game again");
      }
      if (ModalButton("Report a problem")) {
        ImGui::CloseCurrentPopup();
        MakeReport();
      }
      if (ModalButton("Close")) ImGui::CloseCurrentPopup();
      ImGui::NewLine();
      ImGui::EndPopup();
    }
  }

  // ---------------- Report a problem
  void ReportPopup() {
    if (BeginGameModal("report", "REPORT A PROBLEM", 620 * Dpi())) {
      ImGui::TextWrapped("Make a problem report: a .zip you can send us.");
      ImGui::Spacing();
      Note("Inside: the logs of your last 3 games (with which wrestlers, arena and match you played), small "
           "screenshots of each scene, the moments you marked with F11, the performance report, the launcher's "
           "settings, the version and your PC's basics (graphics cards and drivers, CPU, RAM, Windows, monitor). Your "
           "user name is removed from file paths. Nothing is sent: the folder opens and you choose what to do with "
           "it.");
      Note("Tip: press F11 in the game when you see a problem. It marks the moment (a screenshot and what the game "
           "was doing) for this report.");
      ImGui::Spacing();
      ImGui::TextUnformatted("What happened? (optional)");
      ImGui::InputTextMultiline("##report_note", report_note_, sizeof(report_note_),
                                ImVec2(-FLT_MIN, 90 * Dpi()));
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("MAKE THE REPORT")) {
        ImGui::CloseCurrentPopup();
        MakeReport();
      }
      if (ModalButton("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::NewLine();
      ImGui::EndPopup();
    }
  }

  // Replaces the user's profile folder (C:\Users\NAME) in text with %USERPROFILE%.
  static std::string Redact(std::string t) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return t;
    std::string prof = Narrow(buf);
    for (const std::string& needle : {prof, Lower(prof), [&] {  // also with forward slashes
           std::string x = prof;
           std::replace(x.begin(), x.end(), '\\', '/');
           return x;
         }()}) {
      if (needle.size() < 4) continue;
      for (size_t at = 0; (at = t.find(needle, at)) != std::string::npos;) t.replace(at, needle.size(), "%USERPROFILE%");
    }
    // The user name as a whole word anywhere (case-insensitive), e.g. in a folder named after the user (Session 25
    // release check: "C--Users-NAME-..." in a temp folder name). Bounded by non-alphanumerics so a short name doesn't
    // eat parts of ordinary words.
    const size_t slash = prof.find_last_of('\\');
    const std::string user = Lower(slash == std::string::npos ? prof : prof.substr(slash + 1));
    if (user.size() >= 3) {
      const std::string low = Lower(t);
      std::string out;
      size_t from = 0;
      auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
      for (size_t at = low.find(user); at != std::string::npos; at = low.find(user, at + 1)) {
        const bool left = at == 0 || !word(low[at - 1]), right = at + user.size() >= low.size() || !word(low[at + user.size()]);
        if (!left || !right || at < from) continue;
        out.append(t, from, at - from).append("USER");
        from = at + user.size();
      }
      t = out.append(t, from, std::string::npos);
    }
    return t;
  }

  static fs::path Newest(const fs::path& dir, const std::string& prefix, const std::string& ext) {
    fs::path best;
    fs::file_time_type t{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      const std::string n = PathStr(e.path().filename());
      if (!e.is_regular_file() || n.rfind(prefix, 0) != 0 || Lower(PathStr(e.path().extension())) != ext) continue;
      const auto w = e.last_write_time(ec);
      if (best.empty() || w > t) best = e.path(), t = w;
    }
    return best;
  }

  static std::string ReadTail(const fs::path& p, size_t max_bytes) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff size = f.tellg();
    const std::streamoff from = size > std::streamoff(max_bytes) ? size - std::streamoff(max_bytes) : 0;
    f.seekg(from);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    if (from) s = "[... the first " + std::to_string(from) + " bytes left out ...]\n" + s;
    return s;
  }

  // Session 58: every display adapter Windows lists, with its driver version and date, and the CPU's threads / cores.
  static std::string PcDetailsText() {
    std::string t = "Graphics drivers:\n";
    const wchar_t* cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    for (int i = 0; i < 16; ++i) {
      wchar_t key[256];
      swprintf(key, 256, L"%s\\%04d", cls, i);
      const std::string desc = RegString(HKEY_LOCAL_MACHINE, key, L"DriverDesc");
      if (desc.empty()) continue;
      t += "  " + desc + ": driver " + RegString(HKEY_LOCAL_MACHINE, key, L"DriverVersion") + " (" +
           RegString(HKEY_LOCAL_MACHINE, key, L"DriverDate") + ")\n";
    }
    DWORD len = 0;
    uint32_t cores = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<uint8_t> buf(len);
    if (len && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
                                                &len)) {
      for (DWORD off = 0; off < len;) {
        const auto* p = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        ++cores;
        off += p->Size;
      }
    }
    t += "CPU threads: " + std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) + ", cores: " +
         std::to_string(cores) + "\n";
    return t;
  }

  // Session 58: `folder`'s files into a compressed .zip with Windows' own tar.exe (Windows 10 1803 and later).
  static bool TarZip(const fs::path& folder, const fs::path& zip) {
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    const fs::path tar = fs::path(sys) / L"tar.exe";
    std::error_code ec;
    if (!fs::exists(tar, ec)) return false;
    std::wstring cmd = L"\"" + tar.wstring() + L"\" -a -c -f \"" + zip.wstring() + L"\" -C \"" + folder.wstring() + L"\"";
    for (const auto& e : fs::recursive_directory_iterator(folder, ec)) {
      if (e.is_regular_file()) cmd += L" \"" + fs::relative(e.path(), folder, ec).generic_wstring() + L"\"";
    }
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      return false;
    }
    const DWORD w = WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w == WAIT_OBJECT_0 && code == 0 && fs::exists(zip, ec) && fs::file_size(zip, ec) > 0;
  }

  // Session 58: the newest `n` files in `dir` named prefix*ext, newest first.
  static std::vector<fs::path> NewestN(const fs::path& dir, const std::string& prefix, const std::string& ext, size_t n) {
    std::vector<std::pair<fs::file_time_type, fs::path>> all;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      const std::string name = PathStr(e.path().filename());
      if (!e.is_regular_file() || name.rfind(prefix, 0) != 0 || Lower(PathStr(e.path().extension())) != ext) continue;
      all.emplace_back(e.last_write_time(ec), e.path());
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (size_t i = 0; i < all.size() && i < n; ++i) out.push_back(all[i].second);
    return out;
  }

  // Session 58: a game log for the report, condensed: each distinct message (its time stamp and thread id left out)
  // keeps its first 30 copies and the rest are counted at the end (an HD 520's log had one warning tens of thousands
  // of times); a result still over 24 MB keeps its first 4 and last 20 MB.
  static std::string CondenseLog(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string text((std::istreambuf_iterator<char>(f)), {});
    std::unordered_map<std::string, uint32_t> seen;
    std::vector<std::string> repeated;
    std::string out;
    out.reserve(text.size() / 2);
    for (size_t pos = 0; pos < text.size();) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      const std::string_view line(text.data() + pos, nl - pos);
      std::string_view rest = line;
      if (rest.size() > 26 && rest[0] == '[' && rest[1] == '2') rest.remove_prefix(std::min(rest.find("] ") + 2, rest.size()));
      std::string key(rest);
      const size_t t = key.find("] [t");
      if (t != std::string::npos) {
        const size_t e = key.find("] ", t + 3);
        if (e != std::string::npos) key.erase(t + 1, e - t);
      }
      uint32_t& n = seen[key];
      if (++n <= 30) {
        out.append(line).push_back('\n');
      } else if (n == 31) {
        repeated.push_back(key);
      }
      pos = nl + 1;
    }
    if (!repeated.empty()) {
      out += "\n[report] lines left out after their first 30 copies (count left out, message):\n";
      for (const std::string& k : repeated) out += "  " + std::to_string(seen[k] - 30) + "  " + k + "\n";
    }
    constexpr size_t kMax = 24u << 20, kHead = 4u << 20, kTail = 20u << 20;
    if (out.size() > kMax) {
      out = out.substr(0, kHead) + "\n[report] ... " + std::to_string(out.size() - kHead - kTail) +
            " bytes left out ...\n" + out.substr(out.size() - kTail);
    }
    return out;
  }

  // Session 58: "Matches in this log" for about.txt, from its [wwe2k14][match] card lines (ids named from
  // data\ids\characters.json and the DLC packs' names).
  std::string MatchCardsText(const std::string& log) const {
    std::string out;
    std::string last;
    for (size_t at = log.find("[wwe2k14][match] card ("); at != std::string::npos;
         at = log.find("[wwe2k14][match] card (", at + 1)) {
      const size_t nl = log.find('\n', at);
      const std::string line = log.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      const size_t p = line.find("participants ");
      if (p == std::string::npos) continue;
      std::string who;
      for (size_t i = p + 13; i < line.size();) {
        const size_t colon = line.find(':', i);
        if (colon == std::string::npos) break;
        const int slot = std::atoi(line.c_str() + i), id = std::atoi(line.c_str() + colon + 1);
        if (slot < 40) {  // 40-42: the extra ids (roles unknown)
          std::string name = "#" + std::to_string(id);
          for (const auto* list : {&chars_, &dlc_chars_}) {
            for (const Character& c : *list) {
              if (c.id == id) name = c.name + " (" + std::to_string(id) + ")";
            }
          }
          who += (who.empty() ? "" : ", ") + name;
        }
        const size_t comma = line.find(", ", colon);
        if (comma == std::string::npos || comma > line.find(" frame=", colon)) break;
        i = comma + 2;
      }
      std::string arena;
      if (const size_t a = line.find(" arena "); a != std::string::npos) {
        const int id = std::atoi(line.c_str() + a + 7);
        arena = "arena " + std::to_string(id);
        for (const Arena& ar : arenas_) {
          if (ar.id == id) arena = ar.name + " (" + std::to_string(id) + ")";
        }
      }
      const size_t ty = line.find(" type ");
      const std::string entry = "  " + who + "; " + arena + "; match type " +
                                (ty == std::string::npos ? "?" : std::to_string(std::atoi(line.c_str() + ty + 6))) +
                                (line.find("(match clock started)") != std::string::npos ? " (played)" : "") + "\n";
      if (entry != last) out += entry;
      last = entry;
    }
    return out.empty() ? "  (none recorded)\n" : out;
  }

  void MakeReport() {
    fs::path dir = paths_.launcher_dir / "reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) dir = LocalDataDir() / "reports", fs::create_directories(dir, ec);
    const std::string stamp = "report_" + NowStamp("%Y%m%d_%H%M%S");
    const fs::path zip = dir / (stamp + ".zip");
    // Session 58: the files are put together in a folder first, then zipped (compressed) with Windows' own tar.exe;
    // if that fails, the stored .zip of before.
    std::vector<std::pair<std::string, std::string>> files;  // name in the zip -> contents
    std::string about = std::string("WWE 2K14 recomp problem report\nVersion: ") + Version() + "\nMade: " +
                        NowStamp("%Y-%m-%d %H:%M") + " (local time)\n\n" + HardwareText(hw_) + PcDetailsText() +
                        "\nGame data: " + gd_.message + "\nLast game exit: " +
                        (last_exit_.empty() ? std::string("(none since the launcher started)") : last_exit_) + "\n";
    const std::string note(report_note_);
    about += "Player's note: " + (note.empty() ? std::string("(none)") : std::string("note.txt")) + "\n";
    if (!note.empty()) files.emplace_back("note.txt", note + "\n");
    // The last 3 games: logs (condensed), their scene snapshots and F11 marks.
    const fs::path captures = paths_.game_exe.parent_path() / "captures";
    size_t marks = 0;
    about += "\nGames in this report (newest first):\n";
    for (const fs::path& log : NewestN(paths_.logs, "wwe2k14_", ".log", 3)) {
      const std::string name = PathStr(log.filename());
      const std::string text = Redact(CondenseLog(log));
      files.emplace_back(name, text);
      const std::string number = name.substr(8, name.size() - 8 - 4);  // wwe2k14_NNN.log
      about += "- " + name + "\n" + MatchCardsText(text);
      for (const auto& e : fs::directory_iterator(captures, ec)) {
        const std::string c = PathStr(e.path().filename());
        if (c.rfind(number + "_scene_", 0) == 0 && Lower(PathStr(e.path().extension())) == ".jpg") {
          files.emplace_back("captures/" + c, ReadTail(e.path(), 4u << 20));
        } else if (c.rfind(number + "_bookmark_", 0) == 0 && marks < 12) {
          const std::string ext = Lower(PathStr(e.path().extension()));
          if (ext == ".png" || ext == ".jpg" || ext == ".txt") {
            files.emplace_back("captures/" + c, ext == ".txt" ? Redact(ReadTail(e.path(), 1u << 20))
                                                              : ReadTail(e.path(), 16u << 20));
            if (ext != ".txt") ++marks;
          }
        }
      }
    }
    about += "F11 marks: " + std::to_string(marks) + "\n";
    const fs::path perf = Newest(paths_.logs, "perf_report_", ".txt");
    about += "Performance report: " + (perf.empty() ? std::string("(none: turn it on in Settings)") : PathStr(perf.filename())) + "\n";
    if (!perf.empty()) files.emplace_back(PathStr(perf.filename()), Redact(ReadTail(perf, 4u << 20)));
    about += DlcReportText();  // Session 32: pack names and states, never their files
    files.emplace_back("launcher_settings.json", Redact(json(s).dump(2)));
    std::string recent;
    for (const std::string& m : recent_log_) recent += m + "\n";
    files.emplace_back("launcher_messages.txt", Redact(recent));
    files.insert(files.begin(), {"about.txt", Redact(about)});

    bool zipped = false;
    const fs::path stage = dir / stamp;
    fs::create_directories(stage / "captures", ec);
    bool staged = !ec;
    for (const auto& [name, data] : files) {
      if (!staged) break;
      std::ofstream out(stage / Widen(name), std::ios::binary);
      out.write(data.data(), std::streamsize(data.size()));
      staged = bool(out);
    }
    if (staged) zipped = TarZip(stage, zip);
    fs::remove_all(stage, ec);
    if (!zipped) {
      ZipWriter z(zip);
      if (!z.ok()) {
        Status("Could not write " + PathStr(zip));
        return;
      }
      for (const auto& [name, data] : files) z.Add(name, data);
      if (!z.Close()) {
        Status("Could not finish " + PathStr(zip));
        return;
      }
    }
    report_note_[0] = 0;
    last_report_ = zip;
    Status("Report saved: " + PathStr(zip));
    if (open_folders_) {
      const std::wstring arg = L"/select,\"" + zip.wstring() + L"\"";
      ShellExecuteW(hwnd_, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
  }

  // ---------------- Session 39: the performance test (the native renderer on vs off, for a playtester's laptop)
  // Offered only when the test route native_ab.txt is next to the game (only a test build ships it). One game: a
  // Quick Match the route drives by itself (Lesnar vs Bryan, entrances on, both CPU; the native renderer switched on
  // and off every 120 frames through both entrances and the start of the match) on the accurate renderer, which then
  // quits. Its choices are its own flags: the player's saved settings are not changed. Then one .zip of the run's log
  // and frame times (user name removed) to send back; tools\native\laptop_ab.py reads it. (The machinery runs a list
  // of runs; one is enough, Jarrod 2026-10-20.)
  struct PerfRun {
    int renderer;
    const char* label;
    const char* script;  // order A (native on first) or B: alternating windows show different shots, A and B cancel
    std::string log;     // the run's game log (its frames_NNN.csv has the same NNN)
    DWORD code = 0;
  };
  std::vector<PerfRun> perf_runs_;
  int perf_step_ = -1;  // the run going now; -1 = no test running
  bool perf_stop_ = false, want_perf_popup_ = false, perf_offered_ = false;
  fs::path perf_zip_;

  // Session 41 (low-spec step 0, docs\native\LOWSPEC_PLAN.md): the test is now the per-pass breakdown. One button, two
  // games in a row: the LOW preset (NATIVE: the accurate renderer with the native renderer, fast clears; 720p, frame
  // rate 30), then the light renderer with the same settings. The route (perf_test.txt, from
  // tools\input_scripts\short\s41_perf_breakdown.txt) takes frame-anatomy captures with GPU timestamps per pass at the
  // main menu, Bryan's entrance and the match, and switches the fast clears on and off through the entrances. The zip
  // gets the anatomy files too; tools\native\laptop_breakdown.py reads it.
  static constexpr const char* kPerfRoute = "perf_test.txt";
  bool PerfTestAvailable() const {
    std::error_code ec;
    return fs::exists(paths_.game_exe.parent_path() / kPerfRoute, ec);
  }
  void StartPerfTest() {
    if (game_proc_) return Status("Close the game first");
    perf_runs_ = {{2, "LOW preset (native renderer)", kPerfRoute}, {1, "light renderer", kPerfRoute}};
    perf_step_ = 0, perf_stop_ = false, perf_zip_.clear();
    StartPerfRun();
  }
  void StartPerfRun() {
    Settings t = PlainSettings();
    t.renderer = perf_runs_[perf_step_].renderer;
    // The LOW preset's resolution and frame rate for both games (only the renderer differs), no upscaler.
    t.resolution = 0, t.draw_height = 0, t.frame_rate = 3, t.refresh_hz = 0, t.upscale = 0, t.original_look = false;
    t.mods = false, t.cinecam = t.photo_mode = t.runin = false, t.perf_report = 0;
    t.scenario = "@quick", t.quick_type = 0, t.quick_ids = "343,324", t.quick_cpu_only = true, t.quick_arena = -1;
    t.skip_entrances = false;
    // (Session 60: without the match's faster fast-forward vblank - the numbers stay comparable with earlier builds'
    // tests, and its precise timer does not take a core from a weak laptop.)
    t.extra = std::string("--wwe2k14_input_script=") + perf_runs_[perf_step_].script +
              " --wwe2k14_file_trace=true --vsync=false --wwe2k14_ff_vblank_hz=0";
    StartGame(BuildCommand(t, paths_), "Performance test: run " + std::to_string(perf_step_ + 1) + " of " +
                                           std::to_string(perf_runs_.size()) + " started");
    if (!game_proc_) perf_step_ = -1;  // could not start: Status says why
  }
  // From PollGame, when a test run's game has closed.
  void PerfRunEnded(DWORD code) {
    PerfRun& r = perf_runs_[perf_step_];
    r.code = code;
    const fs::path log = Newest(paths_.logs, "wwe2k14_", ".log");
    r.log = log.empty() ? std::string() : PathStr(log.filename());
    if (!perf_stop_ && perf_step_ + 1 < int(perf_runs_.size())) {
      ++perf_step_;
      StartPerfRun();
      return;
    }
    perf_step_ = -1;
    FinishPerfTest();
  }
  void FinishPerfTest() {
    fs::path dir = paths_.launcher_dir / "reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) dir = LocalDataDir() / "reports", fs::create_directories(dir, ec);
    const fs::path zip = dir / ("performance_test_" + NowStamp("%Y%m%d_%H%M") + ".zip");
    ZipWriter z(zip);
    if (!z.ok()) return Status("Could not write " + PathStr(zip));
    std::string about = std::string("WWE 2K14 recomp performance test (Session 41: per-pass breakdown, LOW preset and "
                                    "light renderer)\nVersion: ") +
                        Version() + "\nMade: " + NowStamp("%Y-%m-%d %H:%M") + " (local time)\n\n" + HardwareText(hw_) +
                        "\nTest settings: 720p, frame rate 30 (the LOW preset's), no upscaler; the player's own "
                        "resolution: " +
                        ResolutionName(s) + "\n\n";
    for (size_t i = 0; i < perf_runs_.size(); ++i) {
      const PerfRun& r = perf_runs_[i];
      about += "Run " + std::to_string(i + 1) + ": " + r.label + ", log " + (r.log.empty() ? "(none)" : r.log) +
               ", exit " + (r.log.empty() && !r.code ? std::string("(not run)") : ExitCodeText(r.code)) + "\n";
      if (r.log.empty()) continue;
      z.Add(r.log, Redact(ReadTail(paths_.logs / Widen(r.log), 48u << 20)));
      const std::string number = r.log.substr(8, r.log.size() - 8 - 4);  // wwe2k14_NNN.log
      const std::string frames = "frames_" + number + ".csv";
      if (fs::exists(paths_.logs / Widen(frames), ec)) z.Add(frames, ReadTail(paths_.logs / Widen(frames), 16u << 20));
      // Session 41: the run's frame-anatomy captures (anatomy_NNN_K.csv trimmed to the columns the breakdown reads;
      // its _gpu, _gpu_frames and _frames files whole). Numbers only (shader hashes, registers, times).
      const std::string prefix = "anatomy_" + number + "_";
      for (const auto& e : fs::directory_iterator(paths_.logs, ec)) {
        const std::string name = PathStr(e.path().filename());
        if (name.rfind(prefix, 0) != 0 || e.path().extension() != ".csv") continue;
        const bool main_csv = name.find_first_not_of("0123456789", prefix.size()) == name.size() - 4;
        std::string data = ReadTail(e.path(), 64u << 20);
        if (main_csv) data = AnatomyColumns(data);
        if (name.find("_texloads") == std::string::npos) z.Add(name, data);
      }
    }
    z.Add("about.txt", Redact(about));
    if (!z.Close()) return Status("Could not finish " + PathStr(zip));
    perf_zip_ = zip;
    want_perf_popup_ = true;
    Status("Performance test saved: " + PathStr(zip));
    Log("performance test saved " + PathStr(zip));
  }
  // Session 41: a frame-anatomy CSV with only the columns tools\native\laptop_breakdown.py reads (the zip is stored
  // uncompressed; the full file is ~3 MB per capture). Rows whose field count differs from the header's are left out.
  static std::string AnatomyColumns(const std::string& csv) {
    static const char* kKeep[] = {"frame",          "idx",           "kind",          "cp_us",         "vs_hash",
                                  "ps_hash",        "prim",          "count",         "rb_surface_info",
                                  "rb_color_info0", "rb_color_info1", "rb_color_info2", "rb_color_info3",
                                  "rb_depth_info",  "rb_modecontrol", "rb_depthcontrol", "rb_color_mask", "skipped",
                                  "copy_dest",      "copy_info",     "dispatches",    "barrier_batches",
                                  "fsi_edram_barriers"};
    auto split = [](const std::string& line) {
      std::vector<std::string> f;
      size_t a = 0;
      for (size_t b; (b = line.find(',', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
      f.push_back(line.substr(a));
      return f;
    };
    std::string out;
    std::vector<size_t> cols;
    size_t header_fields = 0;
    size_t pos = 0;
    while (pos < csv.size()) {
      size_t end = csv.find('\n', pos);
      if (end == std::string::npos) end = csv.size();
      std::string line = csv.substr(pos, end - pos);
      pos = end + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      const std::vector<std::string> f = split(line);
      if (!header_fields) {
        header_fields = f.size();
        for (const char* k : kKeep)
          for (size_t i = 0; i < f.size(); ++i)
            if (f[i] == k) cols.push_back(i);
      } else if (f.size() != header_fields) {
        continue;
      }
      for (size_t i = 0; i < cols.size(); ++i) out += (i ? "," : "") + f[cols[i]];
      out += '\n';
    }
    return out;
  }

  void PerfTestPopup() {
    // Offered once when the launcher opens (not to automation runs, which start it with --perf-test if they want it).
    if (PerfTestAvailable() && !perf_offered_) perf_offered_ = true, want_perf_popup_ = steps.empty();
    if (want_perf_popup_) ImGui::OpenPopup("perf_test"), want_perf_popup_ = false;
    if (!BeginGameModal("perf_test", "PERFORMANCE TEST", 680 * Dpi())) return;
    if (perf_step_ >= 0) {
      ImGui::Text("The test is running.");
      ImGui::Spacing();
      ImGui::TextWrapped("The game starts, plays two entrances and the start of a match by itself, and closes; "
                         "then it does the same once more with another renderer (15 to 25 minutes in all on a "
                         "laptop). Please leave the PC alone until then; don't touch the controller or keyboard "
                         "while the game runs.");
      Note("To stop early: close the game (Alt+F4); what ran so far is still saved.");
    } else if (!perf_zip_.empty()) {
      ImGui::TextWrapped("Done, thank you! Please send this file:");
      ImGui::TextColored(kGood, "%s", PathStr(perf_zip_).c_str());
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("OPEN THE FOLDER")) {
        const std::wstring arg = L"/select,\"" + perf_zip_.wstring() + L"\"";
        ShellExecuteW(hwnd_, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
      }
      if (ModalButton("Close")) ImGui::CloseCurrentPopup();
    } else {
      ImGui::TextWrapped("This test build measures where your graphics card spends its time. It plays the same "
                         "Quick Match twice by itself, with two renderers (15 to 25 minutes in all on a laptop), and "
                         "then saves one .zip for you to send back. Your settings stay as they are.");
      ImGui::Spacing();
      Note("Before you start: plug the laptop in, close other programs, and leave it alone while it runs.");
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("START THE TEST", gd_.ok && !game_proc_)) StartPerfTest();
      if (ModalButton("Not now")) ImGui::CloseCurrentPopup();
      if (!gd_.ok) ImGui::TextColored(kBad, "Set up your game files first (Settings).");
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }

  // ---------------- Mods
  // Session 31: game-style tabs: the selected one a slanted black bar with red text (like the selected page on the
  // red panel), the others grey. Returns true when the selection changed.
  bool GameTabs(const char* const* names, int count, int& sel) {
    const float d = Dpi(), px = 21 * d, h = 34 * d, pad = 18 * d, sk = 9 * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int before = sel;
    for (int i = 0; i < count; ++i) {
      if (i) ImGui::SameLine(0, 2 * d);
      const ImVec2 ts = MenuTextSize(px, names[i]), pos = ImGui::GetCursorScreenPos();
      const ImVec2 size(ts.x + pad * 2, h);
      ImGui::PushID(i);
      if (ImGui::InvisibleButton(names[i], size)) sel = i;
      ImGui::PopID();
      const bool on = sel == i, hover = ImGui::IsItemHovered();
      if (on || hover) {
        const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + size.x, pos.y}, {pos.x + size.x - sk, pos.y + h},
                             {pos.x, pos.y + h}};
        dl->AddConvexPolyFilled(q, 4, on ? IM_COL32(0, 0, 0, 255) : IM_COL32(0, 0, 0, 90));
      }
      const ImVec2 tp(pos.x + pad, pos.y + (h - ts.y) / 2);
      if (on) MenuTextRaw(dl, tp, px, kMenuRed, names[i]);
      else MenuText(dl, tp, px, hover ? IM_COL32(205, 206, 211, 255) : IM_COL32(141, 142, 147, 255), names[i]);
    }
    return sel != before;
  }

  static constexpr const char* kModsTabs[] = {"TRONS", "TITLES", "LOGOS"};

  void ModsPage() {
    const float d = Dpi();
    if (want_mods_tab_ >= 0) s.mods_tab = want_mods_tab_, want_mods_tab_ = -1;
    s.mods_tab = std::clamp(s.mods_tab, 0, 2);
    Heading("MODS", 1.6f);
    ImGui::SameLine(0, 28 * d);
    GameTabs(kModsTabs, 3, s.mods_tab);
    if (s.mods_tab < 2) {
      // Trons and titles are both read from the mods folder by --wwe2k14_mods: one switch, on both tabs.
      const float w = 470 * d;
      ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
      OptionToggle("USE MY VIDEOS", s.mods, true, nullptr, w);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(s.mods ? "On: your replacement trons and title reveals play in the game."
                                 : "Off: every original plays; your files stay in the mods folder.");
    }
    ImGui::Spacing();
    if (s.mods_tab == 0) TronsTab();
    else if (s.mods_tab == 1) TitlesTab();
    else LogosTab();
  }

  // ---------------- Logos (Session 25 item 1)
  void RefreshLogos(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - logo_checked_ < std::chrono::seconds(2)) return;
    logo_checked_ = now;
    std::error_code ec;
    const fs::path title = FindTitleDir(paths_.user_data);
    const fs::path file = title.empty() ? fs::path() : title / "00000001" / "00PaintTool.pt" / "SaveData.Dat";
    const auto stamp = file.empty() ? fs::file_time_type{} : fs::last_write_time(file, ec);
    if (!force && logo_loaded_ && title == logo_.title_dir && stamp == logo_stamp_) return;
    logo_ = ReadLogoSave(paths_.user_data);
    logo_stamp_ = stamp, logo_loaded_ = true;
    for (auto*& t : logo_tex_) ReleaseLater(t), t = nullptr;
    for (int i = 0; i < kLogoSlots; ++i) {
      const auto& sl = logo_.slots[size_t(i)];
      if (sl.used && !sl.rgba.empty()) logo_tex_[size_t(i)] = MakeTexture(sl.rgba, sl.w, sl.h);
    }
    backups_ = ListBackups(logo_.title_dir);
  }

  ID3D11ShaderResourceView* PngTexture(const fs::path& p, int* w = nullptr, int* h = nullptr) {
    auto it = png_tex_.find(p);
    if (it == png_tex_.end()) {
      PngTex t;
      FILE* fp = _wfopen(p.c_str(), L"rb");
      int n = 0;
      if (fp) {
        if (unsigned char* px = stbi_load_from_file(fp, &t.w, &t.h, &n, 4)) {
          t.srv = MakeTexture(std::vector<uint8_t>(px, px + size_t(t.w) * t.h * 4), t.w, t.h);
          stbi_image_free(px);
        }
        fclose(fp);
      }
      it = png_tex_.emplace(p, t).first;
    }
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    return it->second.srv;
  }

  // A picture fitted into [a, b] over a checkerboard (transparency shows).
  void DrawPicture(ImDrawList* dl, ID3D11ShaderResourceView* srv, int w, int h, ImVec2 a, ImVec2 b) {
    Checker(dl, a, b, 8 * Dpi());
    if (!srv || w <= 0 || h <= 0) return;
    const float bw = b.x - a.x, bh = b.y - a.y, aspect = float(w) / float(h);
    const float iw = aspect >= bw / bh ? bw : bh * aspect, ih = aspect >= bw / bh ? bw / aspect : bh;
    const ImVec2 i0(a.x + (bw - iw) / 2, a.y + (bh - ih) / 2);
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), i0, ImVec2(i0.x + iw, i0.y + ih));
  }

  void LogosTab() {
    RefreshLogos(false);
    const float dpi = Dpi(), right_w = 430 * dpi;
    if (!logo_.save_exists) {
      ImGui::Dummy(ImVec2(0, 20 * dpi));
      Heading("No save found", 1.2f);
      ImGui::TextWrapped("Start the game once to create your save, then come back. (Play, go past the title screen; "
                         "the game makes its save the first time.)");
      ImGui::TextDisabled("Looked in: %s", PathStr(paths_.user_data).c_str());
      if (ImGui::Button("Look again")) RefreshLogos(true);
      return;
    }
    Note("Your own pictures as real Create-a-Logo logos: the game saves them like logos you paint yourself, so you "
         "can use them on attires, signs and the rest.");
    // Left: the 20 slots as the game shows them (2 pages of 2 x 5).
    ImGui::BeginChild("slots", ImVec2(-right_w - 8, -ImGui::GetFrameHeightWithSpacing() * 1.2f), ImGuiChildFlags_Borders);
    if (!logo_.error.empty()) ImGui::TextColored(kBad, "%s", logo_.error.c_str());
    const float avail = ImGui::GetContentRegionAvail().x, gap = 8 * dpi;
    const float tile = std::min(120 * dpi, (avail - gap * 4) / 5);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int page = 0; page < 2; ++page) {
      ImGui::TextDisabled("Page %d", page + 1);
      for (int k = 0; k < 10; ++k) {
        const int i = page * 10 + k;
        if (k % 5) ImGui::SameLine(0, gap);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + tile, a.y + tile);
        ImGui::Dummy(ImVec2(tile, tile + ImGui::GetTextLineHeight()));
        const auto& sl = logo_.slots[size_t(i)];
        if (sl.used) {
          DrawPicture(dl, logo_tex_[size_t(i)], sl.w, sl.h, a, b);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slot %d: %dx%d, saved %s UTC", i + 1, sl.w, sl.h, sl.date.c_str());
        } else {
          dl->AddRectFilled(a, b, IM_COL32(24, 26, 32, 255), 4 * dpi);
          const char* t = "free";
          const ImVec2 ts = ImGui::CalcTextSize(t);
          dl->AddText(ImVec2(a.x + (tile - ts.x) / 2, a.y + (tile - ts.y) / 2), IM_COL32(90, 95, 110, 255), t);
        }
        dl->AddRect(a, b, sl.used ? IM_COL32(140, 145, 160, 255) : IM_COL32(60, 64, 76, 255), 4 * dpi);
        char lab[32];
        std::snprintf(lab, sizeof(lab), sl.used ? "%d  %dx%d" : "%d", i + 1, sl.w, sl.h);
        dl->AddText(ImVec2(a.x + 2, b.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), lab);
      }
      ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: import and backups.
    ImGui::BeginChild("logoside", ImVec2(right_w, -ImGui::GetFrameHeightWithSpacing() * 1.2f), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Import pictures");
    if (importing_) {
      ImGui::TextColored(kWarn, "Importing %zu logo(s)...", logo_jobs_.size());
      ImGui::TextWrapped("The game is going through the menus for you. Please don't touch the controller or the "
                         "game window; it closes itself when it's done (about %d seconds).",
                         20 + 12 * int(logo_jobs_.size()));
    } else {
      if (ImGui::Button("Add PNG pictures...", ImVec2(-1, 0)))
        for (auto& f : PickPngs(hwnd_)) AddLogoPick(f);
      // Session 53 (a playtester's request): the logo's size, the game's own SELECT CANVAS SIZE choice.
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Logo size:");
      ImGui::SameLine();
      ImGui::RadioButton("256x256", &s.logo_size, 256);
      ImGui::SameLine();
      ImGui::RadioButton("128x128", &s.logo_size, 128);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A smaller logo: the game's 128x128 canvas (the size its own editor offers first).");
      if (logo_picks_.empty()) {
        Note("Any PNG works; it is scaled to the logo size. Square pictures look best.");
      } else {
        // The selected picture, as the game will get it (stretched to the square canvas).
        if (logo_pick_sel_ >= int(logo_picks_.size())) logo_pick_sel_ = 0;
        const fs::path& cur = logo_picks_[size_t(logo_pick_sel_)];
        int w = 0, h = 0;
        ID3D11ShaderResourceView* srv = PngTexture(cur, &w, &h);
        const float side = std::min(256 * dpi, ImGui::GetContentRegionAvail().x);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(side, side));
        // The game's canvas is square: the picture is stretched to it (as src\logo_import.h scales it).
        Checker(ImGui::GetWindowDrawList(), a, ImVec2(a.x + side, a.y + side), 8 * dpi);
        if (srv)
          ImGui::GetWindowDrawList()->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), a,
                                               ImVec2(a.x + side, a.y + side));
        if (srv && w != h) ImGui::TextColored(kWarn, "%dx%d: not square, it will be stretched", w, h);
        else if (!srv) ImGui::TextColored(kBad, "Can't read this PNG");
        for (size_t i = 0; i < logo_picks_.size(); ++i) {
          ImGui::PushID(int(i));
          if (ImGui::SmallButton("x")) {
            logo_picks_.erase(logo_picks_.begin() + std::ptrdiff_t(i));
            ImGui::PopID();
            break;
          }
          ImGui::SameLine();
          if (ImGui::Selectable(PathStr(logo_picks_[i].filename()).c_str(), int(i) == logo_pick_sel_)) logo_pick_sel_ = int(i);
          ImGui::PopID();
        }
      }
      const int free = logo_.Free();
      ImGui::Text("%d free slot(s).", free);
      ImGui::BeginDisabled(logo_picks_.empty() || int(logo_picks_.size()) > free || !gd_.ok || game_proc_ != nullptr);
      char lab[64];
      std::snprintf(lab, sizeof(lab), "Import %zu logo(s)", logo_picks_.size());
      if (BigButton(lab, ImVec2(-1, 40 * dpi))) StartLogoImport();
      ImGui::EndDisabled();
      if (!gd_.ok) ImGui::TextColored(kBad, "Set up your game files first (Settings).");
      Note("Your save is backed up first. The game starts, makes each logo in a free slot and saves it, then "
           "closes. Transparency is kept as on / off (the game stores hard edges). The game allows at most 2 "
           "different 256x256 logos on one custom superstar.");
    }
    if (!logo_result_.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", logo_result_.c_str());
    }

    ImGui::SeparatorText("Backups");
    Note("Before every import the launcher copies your whole save (logos and custom superstars). The last 10 are kept.");
    if (backups_.empty()) ImGui::TextDisabled("No backups yet.");
    ImGui::BeginChild("bk", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 5.5f), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < backups_.size(); ++i)
      if (ImGui::Selectable(backups_[i].label.c_str(), int(i) == backup_sel_)) backup_sel_ = int(i);
    ImGui::EndChild();
    ImGui::BeginDisabled(backup_sel_ < 0 || backup_sel_ >= int(backups_.size()) || game_proc_ != nullptr);
    if (ImGui::Button("Restore...")) ImGui::OpenPopup("restore");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(game_proc_ != nullptr);
    if (ImGui::Button("Back up now")) DoBackup("by hand");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) {
      std::error_code ec;
      fs::create_directories(BackupRoot(), ec);
      ShellExecuteW(hwnd_, L"explore", BackupRoot().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::BeginPopupModal("restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (backup_sel_ >= 0 && backup_sel_ < int(backups_.size())) {
        ImGui::Text("Put this save back?\n  %s", backups_[size_t(backup_sel_)].label.c_str());
        ImGui::TextDisabled("Logos AND custom superstars go back to that moment. Your current save is backed up first.");
        if (ImGui::Button("Restore")) DoRestore(backup_sel_), ImGui::CloseCurrentPopup();
        ImGui::SameLine();
      }
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Save: %s", logo_.title_dir.empty() ? "-" : PathStr(logo_.title_dir).c_str());
  }

  void AddLogoPick(const fs::path& f) {
    if (std::find(logo_picks_.begin(), logo_picks_.end(), f) == logo_picks_.end()) logo_picks_.push_back(f);
  }

  void DoBackup(const std::string& why) {
    RefreshLogos(true);
    const std::string err = MakeBackup(logo_.title_dir, why);
    backups_ = ListBackups(logo_.title_dir);
    Status(err.empty() ? "Save backed up" : "Backup: " + err);
  }

  void DoRestore(int index) {
    if (GameRunning()) {
      Status("Close the game first, then restore");
      return;
    }
    RefreshLogos(true);
    if (index < 0 || index >= int(backups_.size())) return;
    const std::string err = RestoreBackup(backups_[size_t(index)], logo_.title_dir);
    Status(err.empty() ? "Restored the save from " + backups_[size_t(index)].label + " (checked: identical)"
                       : "Restore: " + err);
    Log("restore: " + (err.empty() ? std::string("ok, byte-for-byte identical") : err));
    RefreshLogos(true);
  }

  void StartLogoImport() {
    if (GameRunning()) {
      logo_result_ = "Close the game first: the import runs it by itself.";
      return;
    }
    RefreshLogos(true);
    if (!logo_.save_exists) return;
    if (!logo_.error.empty()) {
      logo_result_ = "Can't read your logo save (" + logo_.error + "); nothing was changed.";
      return;
    }
    std::string err;
    logo_jobs_ = PlanLogoJobs(logo_, logo_picks_, err, s.logo_size);
    if (!logo_forced_.empty() && !logo_jobs_.empty()) {  // automation: the slots given with --logo-add PNG@SLOT
      for (auto& j : logo_jobs_)
        if (auto it = logo_forced_.find(j.png); it != logo_forced_.end()) j.slot = it->second;
      std::sort(logo_jobs_.begin(), logo_jobs_.end(), [](const LogoJob& a, const LogoJob& b) {
        return SlotX(a.slot) != SlotX(b.slot) ? SlotX(a.slot) < SlotX(b.slot) : SlotRow(a.slot) < SlotRow(b.slot);
      });
      std::error_code ec2;
      for (const auto& j : logo_jobs_)
        if (j.slot < 0 || j.slot >= kLogoSlots || logo_.slots[size_t(j.slot)].used ||
            (j.slot == 0 && !fs::exists(logo_.file, ec2)))  // Session 27: a new logo save puts the default logo there
          logo_jobs_.clear(), err = "a forced slot is not free";
      if (logo_jobs_.empty()) {
        logo_result_ = err;
        return;
      }
    }
    if (logo_jobs_.empty()) {
      logo_result_ = err.empty() ? "Nothing to import." : err;
      return;
    }
    // Review fix (Session 27): the old counted route can't answer the "create the Custom Logo save" prompt.
    if (std::error_code lec; s.logo_legacy_route && !fs::exists(logo_.file, lec)) {
      logo_result_ = "The old logo route (Settings -> Advanced) can't create your first logo save: turn it off, or "
                     "open CREATIONS -> LOGO in the game once.";
      return;
    }
    if (std::string e = MakeBackup(logo_.title_dir, "before logo import"); !e.empty()) {
      logo_result_ = "Not imported: the backup failed (" + e + ").";
      return;
    }
    logo_before_ = logo_;
    std::error_code ec;
    fs::create_directories(LocalDataDir(), ec);
    const fs::path script = LocalDataDir() / "logo_import_script.txt";
    std::ofstream(script, std::ios::trunc) << LogoImportScript(logo_jobs_, s.logo_legacy_route, fs::exists(logo_.file, ec));
    // Session 27: the script writes where it ended (ok / failed + the stage) here; read in FinishLogoImport.
    logo_result_file_ = LocalDataDir() / "logo_import_result.txt";
    fs::remove(logo_result_file_, ec);
    Command c;
    c.exe = paths_.game_exe.wstring(), c.workdir = paths_.launcher_dir.wstring();
    std::string a = HelperPathFlags();
    a += "--wwe2k14_input_script=" + Quote(PathStr(script)) + " --vsync=false";  // fast-forwarded menus
    a += " --wwe2k14_use_launcher_settings=false";  // a helper run keeps the default look, whatever the Play settings are
    a += " --wwe2k14_input_result=" + Quote(PathStr(logo_result_file_));
    c.args = Widen(a), c.display = Quote(PathStr(paths_.game_exe)) + " " + a;
    HANDLE h = nullptr;
    if (std::string e = StartProcess(c, &h); !e.empty()) {
      logo_result_ = "Could not start the game: " + e;
      return;
    }
    game_proc_ = h, last_cmd_ = c, importing_ = true;
    logo_result_.clear();
    Log("logo import: " + std::to_string(logo_jobs_.size()) + " job(s), " + c.display);
    backups_ = ListBackups(logo_.title_dir);
  }

  void FinishLogoImport(DWORD code) {
    importing_ = false;
    RefreshLogos(true);
    const LogoResult r = CheckLogoJobs(logo_jobs_, logo_before_, logo_);
    logo_result_ = std::to_string(r.ok) + " of " + std::to_string(logo_jobs_.size()) + " logo(s) imported.\n" + r.detail;
    // Session 27: the route stops (and the game closes without saving) when a screen is not the expected one.
    std::string line;
    if (std::ifstream rf(logo_result_file_); rf && std::getline(rf, line) && line.rfind("failed", 0) == 0) {
      const size_t t = line.rfind('\t');
      std::string stage = t == std::string::npos ? line : line.substr(t + 1);
      if (stage.rfind("logo_import: ", 0) == 0) stage = stage.substr(13);
      logo_result_ += "The import stopped at an unexpected screen (" + stage +
                      "); the game closed without saving there. Nothing else was changed.\n";
      // Session 57 (tester bug 2): before the main menu, the likely screen is the game's notice that the save needs
      // DLC packs it does not see (a pack switched off, or the DLC folder moved).
      if (stage == "start" || stage == "waiting for the main menu")
        logo_result_ += "If the game said your save needs downloadable content: turn on its DLC packs in Settings -> "
                        "GAME (USE MY DLC and every pack the save was made with), then import again.\n";
    } else if (line.empty() && r.failed) {
      logo_result_ += "The game closed before the import finished.\n";
    }
    if (r.failed) logo_result_ += "Your backup from just before the import is in the list below.";
    if (code != 0) logo_result_ += "\nThe game stopped with " + ExitCodeText(code) + ".";
    if (r.failed == 0) logo_picks_.clear();
    Log("logo import result: " + logo_result_);
    Status(r.failed ? "Logo import: some logos were not saved" : "Logo import done");
  }

  // ---------------- Settings
  void CheckGameDataNow() {
    gd_ = CheckGameData(paths_.game_data);
    gd_path_ = paths_.game_data;
  }

  void GameDataSection(bool wizard) {
    const float dpi = Dpi();
    ImGui::TextWrapped("Your game files: the folder you copied from your own disc (it has default.xex in it), or a "
                       "disc image (.iso) of your disc.");
    std::string shown = s.game_data_dir.empty() ? PathStr(paths_.game_data) : s.game_data_dir;
    ImGui::SetNextItemWidth(-230 * dpi);
    if (ImGui::InputTextWithHint("##gd", "e.g. D:\\Games\\WWE2K14 or D:\\Games\\WWE2K14.iso", &shown,
                                 ImGuiInputTextFlags_EnterReturnsTrue))
      s.game_data_dir = shown, Reload();
    ImGui::SameLine();
    if (ImGui::Button("Folder...", ImVec2(110 * dpi, 0)))
      if (auto d = PickFolder(hwnd_, L"Pick the folder with default.xex")) s.game_data_dir = PathStr(*d), Reload();
    ImGui::SameLine();
    if (ImGui::Button("Disc image...", ImVec2(-1, 0)))
      if (auto f = PickDiscImage(hwnd_)) s.game_data_dir = PathStr(*f), Reload();
    ImGui::TextColored(gd_.ok ? kGood : kBad, "%s", gd_.message.c_str());
    if (!wizard && gd_.ok) {
      RefreshLogos(false);
      if (logo_.save_exists) ImGui::TextColored(kGood, "Save found (%d of 20 logo slots used).", kLogoSlots - logo_.Free());
      else ImGui::TextColored(kWarn, "No save yet: the game makes one the first time you play.");
    }
  }

  // Session 31: Settings like the game's OPTIONS: the sections as a list on the left, the section's rows beside it.
  static constexpr const char* kSettingsSections[] = {"GAME", "DISPLAY", "GRAPHICS", "ADVANCED"};
  int settings_section_ = 0;

  void SettingsPage() {
    const float d = Dpi(), list_w = 190 * d;
    Heading("SETTINGS", 1.6f);
    ImGui::Spacing();
    ImGui::BeginGroup();
    for (int i = 0; i < int(std::size(kSettingsSections)); ++i)
      if (ListItem(kSettingsSections[i], i == settings_section_, 21 * d, list_w)) settings_section_ = i;
    ImGui::EndGroup();
    ImGui::SameLine(0, 16 * d);
    ImGui::BeginChild("section", ImVec2(std::min(ImGui::GetContentRegionAvail().x, 720 * d), -4 * d));
    switch (settings_section_) {
      case 0: SettingsGame(); break;
      case 1: SettingsDisplay(); break;
      case 2: SettingsGraphics(); break;
      default: AdvancedSettings(); break;
    }
    ImGui::EndChild();
  }

  // Session 31: the game's own languages (runtime user_language ids, launcher_core.h AutoLanguage).
  static constexpr int kLanguageIds[] = {0, 1, 3, 4, 5, 6};
  static constexpr const char* kLanguageNames[] = {"AUTOMATIC", "ENGLISH",  "DEUTSCH", "FRAN\xC3\x87" "AIS",
                                                   "ESPA\xC3\x91" "OL", "ITALIANO"};

  // ---------------- DLC (Session 32; Jarrod: in Settings -> GAME, beside the game files)
  // The packs in the DLC folder (dlc_catalog.h Scan), refreshed at most every `max_age_s` seconds or when the folder
  // changes. Scanning reads each pack's catalog and checks its listed files: a few hundred file checks.
  std::vector<wwe2k14::dlc::Pack> dlc_packs_;
  fs::path dlc_scanned_;
  std::chrono::steady_clock::time_point dlc_checked_{};
  bool dlc_folder_exists_ = false;

  void RefreshDlc(int max_age_s) {
    const auto now = std::chrono::steady_clock::now();
    if (dlc_scanned_ == paths_.dlc && dlc_checked_.time_since_epoch().count() &&
        now - dlc_checked_ < std::chrono::seconds(max_age_s))
      return;
    dlc_checked_ = now, dlc_scanned_ = paths_.dlc;
    std::error_code ec;
    dlc_folder_exists_ = fs::is_directory(paths_.dlc, ec);
    try {
      dlc_packs_ = dlc_folder_exists_ ? wwe2k14::dlc::Scan(paths_.dlc) : std::vector<wwe2k14::dlc::Pack>{};
    } catch (...) {  // review: never let an odd folder take the launcher down
      dlc_packs_.clear();
    }
    RebuildDlcWrestlers();
  }

  // Session 32 (Jarrod: DLC wrestlers in Quick Match): every ready pack's wrestlers (its catalog's character files),
  // named from the packs' own string files (dlc_names.h; read once per pack and kept). Whether a pack is on is checked
  // where the list is used (Wrestlers()), so a switch takes effect at once.
  std::vector<Character> dlc_chars_;
  std::map<std::string, std::map<int, std::string>> dlc_names_;  // pack root | string file -> names

  void RebuildDlcWrestlers() {
    dlc_chars_.clear();
    std::map<int, std::string> names;  // the string files are cumulative: any pack's list will do
    for (const auto& p : dlc_packs_) {
      if (p.status != wwe2k14::dlc::Status::kReady || p.string_pack.empty()) continue;
      const std::string key = wwe2k14::dlc::PathUtf8(p.root) + "|" + p.string_pack;
      auto it = dlc_names_.find(key);
      if (it == dlc_names_.end()) {
        std::map<int, std::string> got;
        try {
          got = wwe2k14::dlc::NamesFromPack(p);
        } catch (...) {
        }
        it = dlc_names_.emplace(key, std::move(got)).first;
      }
      for (const auto& [id, n] : it->second) names.emplace(id, n);
    }
    for (const auto& p : dlc_packs_) {
      if (p.status != wwe2k14::dlc::Status::kReady) continue;
      for (int id : p.char_ids) {
        const bool known = std::any_of(chars_.begin(), chars_.end(), [&](const Character& c) { return c.id == id; }) ||
                           std::any_of(dlc_chars_.begin(), dlc_chars_.end(), [&](const Character& c) { return c.id == id; });
        if (known) continue;
        Character c;
        c.id = id, c.kind = "wrestler", c.dlc_pack = p.pack_id;
        const auto n = names.find(id);
        c.name = n != names.end() && !n->second.empty() ? wwe2k14::dlc::ReadableName(n->second)
                                                         : "DLC wrestler #" + std::to_string(id);
        c.note = "DLC pack " + std::to_string(p.pack_id);
        dlc_chars_.push_back(std::move(c));
      }
    }
    std::sort(dlc_chars_.begin(), dlc_chars_.end(),
              [](const Character& a, const Character& b) { return Lower(a.name) < Lower(b.name); });
  }
  // A DLC wrestler the game has right now (USE MY DLC on, the pack not switched off).
  bool DlcWrestlerOn(const Character& c) const { return s.dlc && c.dlc_pack >= 0 && !DlcIsOff(c.dlc_pack); }
  bool DlcIsOff(int id) const {
    const auto off = wwe2k14::dlc::ParseIdList(s.dlc_off);
    return std::find(off.begin(), off.end(), id) != off.end();
  }
  void SetDlcOff(int id, bool off) {
    auto ids = wwe2k14::dlc::ParseIdList(s.dlc_off);
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    if (off) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    s.dlc_off.clear();
    for (int v : ids) s.dlc_off += (s.dlc_off.empty() ? "" : ",") + std::to_string(v);
  }
  // Ready packs the game gets (USE MY DLC on, not switched off).
  int DlcPacksOn() const {
    if (!s.dlc) return 0;
    int n = 0;
    for (const auto& p : dlc_packs_) n += p.status == wwe2k14::dlc::Status::kReady && !DlcIsOff(p.pack_id);
    return n;
  }
  static std::string DlcStatusText(const wwe2k14::dlc::Pack& p) {
    using wwe2k14::dlc::Status;
    switch (p.status) {
      case Status::kReady: return "Ready";
      case Status::kIncomplete: return "Incomplete: " + p.reason;
      case Status::kDamaged: return "Damaged: " + p.reason;
      case Status::kDuplicate: return "Duplicate: " + p.reason;
      case Status::kOtherContent: return "Not WWE 2K14 DLC: " + p.reason;
    }
    return "?";
  }
  static std::string SizeText(uint64_t bytes) {
    char b[32];
    if (bytes >= (1u << 20)) std::snprintf(b, sizeof(b), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024) std::snprintf(b, sizeof(b), "%.0f KB", double(bytes) / 1024.0);
    else std::snprintf(b, sizeof(b), "%u bytes", unsigned(bytes));
    return b;
  }
  // The text for Report a problem: pack names and states only (never files).
  std::string DlcReportText() {
    RefreshDlc(0);
    std::string t = std::string("DLC: ") + (s.dlc ? "on" : "off") + ", folder " + PathStr(paths_.dlc) +
                    (dlc_folder_exists_ ? "" : " (not there)") + ", " + std::to_string(dlc_packs_.size()) + " pack(s)\n";
    for (const auto& p : dlc_packs_)
      t += "  pack " + std::to_string(p.pack_id) + " '" + p.folder + "' (" + p.AllNames() + "): " + DlcStatusText(p) +
           (p.status == wwe2k14::dlc::Status::kReady ? (DlcIsOff(p.pack_id) ? ", switched off" : ", on") : "") + "\n";
    return t;
  }

  void DlcSection() {
    const float d = Dpi();
    RefreshDlc(10);  // and at once on Look again / a new folder
    OptionToggle("USE MY DLC", s.dlc, true,
                 "Your own downloadable content, read straight from your files (nothing is copied or changed): the "
                 "packages as copied from your console's drive, or packs you extracted (one folder each, with "
                 "info\\catalog.dlc in it).");
    std::string shown = s.dlc_dir.empty() ? PathStr(paths_.dlc) : s.dlc_dir;
    ImGui::SetNextItemWidth(-230 * d);
    if (ImGui::InputTextWithHint("##dlc", "e.g. D:\\Games\\WWE2K14\\dlc", &shown, ImGuiInputTextFlags_EnterReturnsTrue))
      s.dlc_dir = shown, Reload(), RefreshDlc(0);
    ImGui::SameLine();
    if (ImGui::Button("Folder...##dlc", ImVec2(110 * d, 0)))
      if (auto f = PickFolder(hwnd_, L"Pick the folder with your DLC packs")) s.dlc_dir = PathStr(*f), Reload(), RefreshDlc(0);
    ImGui::SameLine();
    if (ImGui::Button("Look again", ImVec2(-1, 0))) RefreshDlc(0);
    if (!s.dlc_dir.empty()) {
      if (ImGui::SmallButton("Use the usual folder")) s.dlc_dir.clear(), Reload(), RefreshDlc(0);
      ImGui::SameLine();
      ImGui::TextDisabled("(dlc next to your game files)");
    }
    if (!dlc_folder_exists_) {
      ImGui::TextColored(kWarn, "No DLC folder here. To add DLC, make it and copy your packs in, or pick another folder.");
      return;
    }
    int ready = 0;
    for (const auto& p : dlc_packs_) ready += p.status == wwe2k14::dlc::Status::kReady;
    if (dlc_packs_.empty()) {
      ImGui::TextColored(kWarn, "No packs in this folder (a pack is a package from the console's drive, or a folder "
                                "with info\\catalog.dlc in it).");
      return;
    }
    ImGui::TextColored(ready ? kGood : kWarn, "%d pack(s) found, %d on. Changes apply the next time the game starts.",
                       int(dlc_packs_.size()), DlcPacksOn());
    // Session 32 (runs 2443-2477, copies of a save): a save made with packs on keeps needing them; with one off the
    // game says the save can't be used, but the unlocks stayed, even after saving, and came back with the packs on.
    Note("Your save remembers the packs it was made with: with one of them off, the game warns that the save needs "
         "it. Switch the pack back on and everything is there again.");
    // One row per pack: its own name (the pack's main entry), ON / OFF; the grey line has every item, size, state.
    const float w = ImGui::GetContentRegionAvail().x, label_room = w - std::min(300 * d, w * 0.5f) - 30 * d;
    for (const auto& p : dlc_packs_) {
      std::string label = Upper(p.ShortName());
      while (label.size() > 4 && MenuTextSize(20 * d, label.c_str()).x > label_room) label.resize(label.size() - 4), label += "...";
      const std::string names = p.entries.size() > 1 || p.package ? p.AllNames() : "";
      std::string note = (names.empty() ? "" : names + ". ") + SizeText(p.bytes) + (p.package ? " package. " : ". ") +
                         DlcStatusText(p) + ".";
      if (p.folder != ".") note += "  (" + p.folder + ")";
      ImGui::PushID(p.folder.c_str());
      if (p.status == wwe2k14::dlc::Status::kReady) {
        bool on = !DlcIsOff(p.pack_id);
        if (OptionToggle(label.c_str(), on, s.dlc, note.c_str())) SetDlcOff(p.pack_id, !on);
      } else {
        static constexpr const char* kNotReady[] = {"NOT USED"};
        int zero = 0;
        OptionRow(label.c_str(), zero, kNotReady, 1, false, note.c_str());
      }
      ImGui::PopID();
    }
  }

  void SettingsGame() {
    SubHeading("GAME FILES");
    GameDataSection(false);
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    // Session 53 (a playtester's request; Jarrod: "a toggle in the game settings").
    SubHeading("STARTING THE GAME");
    OptionToggle("QUICK START", s.quick_start, true,
                 "Opening the launcher starts the game straight away with these settings (3 seconds to press a key or "
                 "click to stay; hold Shift while opening the launcher to skip it). The launcher closes with the game.");
    if (ImGui::Button("Make a desktop shortcut")) CreateDesktopShortcut();
    ImGui::SameLine();
    ImGui::TextDisabled("It starts the game directly with these settings (so does wwe2k14.exe itself).");
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    SubHeading("DOWNLOADABLE CONTENT");
    DlcSection();
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    int li = 0;
    for (int i = 0; i < int(std::size(kLanguageIds)); ++i)
      if (kLanguageIds[i] == s.language) li = i;
    const int auto_id = AutoLanguage();
    std::string note = "The game's menus and text. Automatic follows Windows (now: ";
    for (int i = 1; i < int(std::size(kLanguageIds)); ++i)
      if (kLanguageIds[i] == auto_id) note += kLanguageNames[i];
    note += "). The game has these five.";
    if (OptionRow("LANGUAGE", li, kLanguageNames, int(std::size(kLanguageNames)), true, note.c_str()))
      s.language = kLanguageIds[li];
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    SubHeading("IN THE GAME");
    OptionToggle("CINECAM", s.cinecam, true, "Free camera: F8 in a match.");
    OptionToggle("RUN-INS", s.runin, true, "F9 in a match: another wrestler interferes (experimental).");
    OptionToggle("PHOTO MODE", s.photo_mode, true,
                 "F10 (or both sticks clicked, held) in a match: the scene nearly stops, a free camera, no HUD, depth "
                 "of field; A / Enter saves a PNG to Pictures\\WWE 2K14.");
    // Session 33: Discord status (the Discord app on this PC; only the wrestlers, the match type and the time).
    OptionToggle("DISCORD STATUS", s.discord, true,
                 "Your Discord status shows the match: the wrestlers, the match type and the time (\"In the menus\" "
                 "otherwise). Needs the Discord app running on this PC. Nothing else is sent.");
    if (s.discord && !*kDiscordAppId) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * Dpi());
      ImGui::SetNextItemWidth(320 * Dpi());
      ImGui::InputTextWithHint("##discord_app", "Discord application id", &s.discord_app_id);
      if (s.discord_app_id.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "No application id yet: nothing is sent.");
      }
    }
    bool pr = s.perf_report < 0 ? g_perf_report_default : s.perf_report == 1;
    if (OptionToggle("PERFORMANCE REPORT", pr, true,
                     "Frame and load times per scene when the game closes; it goes into Report a problem."))
      s.perf_report = pr ? 1 : 0;
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    SubHeading("UPDATES");
    OptionToggle("CHECK FOR UPDATES", s.check_updates, true,
                 "When the launcher starts it asks GitHub whether a newer full release is out (nothing else is sent).");
    if (ImGui::Button("Check now")) StartUpdateCheck(true);
    ImGui::SameLine();
    const UpdateInfo u = Update();
    switch (u.state) {
      case UpdateInfo::kChecking: ImGui::TextDisabled("Checking..."); break;
      case UpdateInfo::kLatest: ImGui::TextColored(kGood, "You have the latest version (%s).", MyVersion().c_str()); break;
      case UpdateInfo::kAvailable:
        ImGui::TextColored(kWarn, "%s is out.", u.tag.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Details")) want_update_popup_ = true, update_dismissed_ = false;
        break;
      case UpdateInfo::kFailed: ImGui::TextColored(kBad, "Couldn't check: %s.", u.error.c_str()); break;
      default: ImGui::TextDisabled("Not checked yet (%s).", MyVersion().c_str()); break;
    }
  }

  void SettingsDisplay() {
    const float d = Dpi();
    // Graphics card: automatic (the game picks the dedicated GPU: src\gpu_pick.h) or one by name.
    if (gpus_.empty()) gpus_ = ListGpus();
    std::vector<std::string> names{"AUTOMATIC"};
    for (const Gpu& g : gpus_) names.push_back(g.name);
    int gi = 0;
    for (size_t i = 0; i < gpus_.size(); ++i)
      if (gpus_[i].name == s.gpu) gi = int(i) + 1;
    std::vector<const char*> ptrs;
    for (const auto& n : names) ptrs.push_back(n.c_str());
    const std::string gnote = gpus_.empty() ? std::string("No graphics card found.")
                                            : "Automatic: the dedicated graphics card with the most memory (here: " +
                                                  gpus_.front().name + ").";
    if (OptionRow("GRAPHICS CARD", gi, ptrs.data(), int(ptrs.size()), true, gnote.c_str()))
      s.gpu = gi == 0 ? std::string() : gpus_[size_t(gi - 1)].name;

    static constexpr const char* kMode[] = {"FULLSCREEN", "WINDOWED"};
    int mode = s.windowed ? 1 : 0;
    if (OptionRow("DISPLAY", mode, kMode, 2)) s.windowed = mode == 1;

    // Session 33: ultrawide. AUTO = the monitor's shape: wider than 16:9 shows more of the scene at the sides; the
    // HUD and menus stay a centred 16:9.
    static constexpr const char* kAspect[] = {"16:9", "AUTO"};
    const double mon = hw_.screen_h ? double(hw_.screen_w) / double(hw_.screen_h) : 16.0 / 9.0;
    char anote[200];
    std::snprintf(anote, sizeof(anote), "%s Your monitor: %dx%d (%.2f:1).",
                  s.aspect == 1 ? "The monitor's shape: more of the arena at the sides on an ultrawide; HUD and menus "
                                  "stay 16:9."
                                : "The game's own 16:9.",
                  hw_.screen_w, hw_.screen_h, mon);
    s.aspect = std::clamp(s.aspect, 0, 1);
    OptionRow("ASPECT", s.aspect, kAspect, 2, true, anote);

    const bool dlss = (s.upscale >= 2 && s.upscale <= 4) || s.upscale == 6 || s.upscale == 7;  // S33: or FSR Q / P
    static constexpr const char* kRes[] = {"720P", "1440P", "4K"};
    const int sug = SuggestResolution(hw_);
    char rnote[200];
    std::snprintf(rnote, sizeof(rnote), "%sYour monitor: %dx%d at %d Hz; suggested %s.",
                  s.original_look ? "Original look is on (Graphics): 720p. "
                  : dlss          ? "The upscaler picks the size itself (Graphics). "
                                  : "",
                  hw_.screen_w, hw_.screen_h, hw_.refresh_hz, kRes[sug]);
    ResolutionRow(s, !dlss && !s.original_look, rnote);

    FrameRateRow(s);
    ImGui::Dummy(ImVec2(0, 10 * d));
    ImGui::TextDisabled("In the game: F3 shows the frame rate.");
  }

  // Session 54 (Jarrod, 2026-10-23: "360p, 480, 540p, 720p, 1080p, 1440p, and 4k as resolution options"): 360p - 540p
  // and 1080p are drawn by NATIVE only (its passes at that size, its copies at 720p / 1440p: gfx_settings.h
  // kResolutions), so the other renderers and the upscalers offer the whole sizes, with a note.
  void ResolutionRow(Settings& t, bool enabled, const char* extra_note) {
    const bool native = DrawHeightApplies(t);
    std::vector<int> entries;  // kResolutions indices offered
    for (int i = 0; i < wwe2k14::gfxs::kResolutionCount; ++i)
      if (native || !wwe2k14::gfxs::kResolutions[i].native_only) entries.push_back(i);
    std::vector<const char*> names;
    for (int i : entries) names.push_back(wwe2k14::gfxs::kResolutions[i].name);
    const int current = wwe2k14::gfxs::ResolutionIndex(t.resolution, t.draw_height, native);
    int vi = 0;
    for (size_t k = 0; k < entries.size(); ++k)
      if (entries[k] == current) vi = int(k);
    const auto& e = wwe2k14::gfxs::kResolutions[entries[size_t(vi)]];
    std::string note = extra_note ? extra_note : "";
    if (enabled && native && e.draw_height) {
      char drawn[160];
      std::snprintf(drawn, sizeof(drawn), " Drawn at %dx%d and scaled to your screen%s.", (e.draw_height * 16 + 8) / 9,
                    e.draw_height, e.draw_height < 720 ? ": less work for the graphics card, softer picture" : "");
      note += drawn;
    }
    if (enabled && !native)
      note += t.renderer != 2 ? " 360p, 480p, 540p and 1080p need the NATIVE renderer."
                              : " 360p, 480p, 540p and 1080p are not available with upscaling.";
    if (OptionRow("RESOLUTION", vi, names.data(), int(names.size()), enabled, note.c_str())) {
      const auto& n = wwe2k14::gfxs::kResolutions[entries[size_t(std::clamp(vi, 0, int(entries.size()) - 1))]];
      t.resolution = n.resolution, t.draw_height = n.draw_height;
    }
  }

  // Session 31 (Jarrod): FRAME RATE = 60 / 120 / UNCAPPED, 60 fps entrances by default. Stored in the fields the game's
  // own GRAPHICS menu shares (gfx_settings.h): 60 = frame_rate 1; 120 = high refresh (2) at 120 steps; UNCAPPED = high
  // refresh at the monitor's rate (refresh_hz 0). The game's speed is tied to its steps, so "uncapped" is as fast as
  // the monitor shows: a real uncapped rate fast-forwards the game (--vsync=false). CLASSIC (0: 30 fps entrances)
  // appears only when it was picked in the game's menu.
  void FrameRateRow(Settings& t) {
    // Session 37: "30" (frame_rate 3) for weak PCs, after UNCAPPED; CLASSIC (0) last when it was picked in the game.
    std::vector<const char*> names{"60", "120", "UNCAPPED", "30"};
    int fi = t.frame_rate == 2 ? (t.refresh_hz == 120 ? 1 : 2) : t.frame_rate == 3 ? 3 : 0;
    if (t.frame_rate == 0) names.push_back("CLASSIC"), fi = 4;
    const int hz = hw_.refresh_hz;
    std::string note = fi == 0   ? "Entrances and matches at 60."
                       : fi == 1 ? "Entrances and matches at 120."
                       : fi == 2 ? "Matches at your monitor's rate (" + std::to_string(hz) + " Hz), entrances at up to 120."
                       : fi == 3 ? "Entrances and matches at 30, for weaker PCs (below 60 the game would run in slow "
                                   "motion). The match timer counts at half rate."
                                 : "Entrances at 30, matches at 60 (picked in the game's own menu).";
    if ((fi == 1 || fi == 2) && hz <= 60) note += " Your monitor runs at " + std::to_string(hz) + " Hz: this needs a faster one.";
    if (OptionRow("FRAME RATE", fi, names.data(), int(names.size()), true, note.c_str())) {
      if (fi == 0) t.frame_rate = 1, t.refresh_hz = 0;
      else if (fi == 1) t.frame_rate = 2, t.refresh_hz = 120;
      else if (fi == 2) t.frame_rate = 2, t.refresh_hz = 0;
      else if (fi == 3) t.frame_rate = 3, t.refresh_hz = 0;
    }
  }

  // Session 37: ACCURATE (the Xbox 360's EDRAM in every pixel shader: the reference look, heavy on weak GPUs) or LIGHT
  // (the graphics card's own render targets with the Session 37 fixes: same picture in our checks; src\light_renderer.h).
  // A card without interlock (older AMD drivers / graphics; Session 54) cannot run ACCURATE: the SDK falls back to LIGHT's path, and
  // --wwe2k14_fbo_fallback_fix (default on, Jarrod 2026-10-20) applies LIGHT's fixes there.
  // Session 41 (low-spec step 2): NATIVE (2) = ACCURATE with the native renderer drawing the scene (the game's own
  // shaders on the card's own render targets, --gpu_native=2): on the playtester's UHD 620 +10-28% fps over ACCURATE.
  // It needs interlock like ACCURATE (the SDK turns it off by itself otherwise).
  // Session 51 (Jarrod, 2026-10-22: "native should just replace low ... it may be easy for playtesters to use a wrong
  // setting"; "NATIVE for everyone"): NATIVE is the default on every PC, ACCURATE and LIGHT are fallbacks, and the LOW
  // preset button is gone (resolution and frame rate are their own rows; integrated graphics still start at 720p / 30).
  // Session 61 (Jarrod, 2026-10-25: "remove the renderer option entirely, with light and accurate as advanced flags
  // only"): no RENDERER row - NATIVE always; --wwe2k14_renderer=light|accurate in Advanced -> Extra flags for
  // troubleshooting (launcher_core.h BuildFlags, gfx_settings.h RendererFromFlags).

  void SettingsGraphics() {
    const float d = Dpi();
    // Shown in Jarrod's order (Session 31): DLAA, then DLSS from the fastest to the sharpest. Session 33: then AMD FSR 3
    // (native, quality, performance) on every card (AMD's runtime ships next to the game). kUpValue = the stored
    // s.upscale (0 off, 1 DLAA, 2 DLSS performance, 3 quality, 4 ultra performance, 5 FSR native, 6 FSR quality, 7 FSR
    // performance; launcher_core.h).
    static constexpr int kUpValue[] = {0, 1, 4, 2, 3, 5, 6, 7};
    static constexpr const char* kUp[] = {"OFF",          "DLAA",        "DLSS ULTRA PERFORMANCE", "DLSS PERFORMANCE",
                                          "DLSS QUALITY", "FSR NATIVE",  "FSR QUALITY",            "FSR PERFORMANCE"};
    static constexpr const char* kUpNote[] = {"No upscaling.", "NVIDIA anti-aliasing at your resolution.",
                                              "720p drawn, shown as 4K.", "720p drawn, shown as 1440p.",
                                              "1440p drawn, shown as 4K.", "AMD anti-aliasing at your resolution.",
                                              "AMD FSR 3: 1440p drawn, shown as 4K.",
                                              "AMD FSR 3: 720p drawn, shown as 1440p."};
    const bool rtx = hw_.rtx || force_rtx_;
    const bool fsr = wwe2k14::gfxs::HasFsrRuntime();
    // Session 37: the card the game will use (Display -> GRAPHICS CARD).
    const wwe2k14::vkdev::Device dev = ChosenDevice(s.gpu);
    // Session 37 (FSR never breaks a game; only where it works): no FSR on integrated graphics (AMD's FSR 3.1 list
    // starts at Radeon RX 5000 / GeForce RTX 20, no integrated GPUs; a playtester's UHD 620 shook and hung with it).
    // Every FSR mode on any monitor otherwise (Jarrod, 2026-10-20: a bigger output than the screen is a choice, like a
    // render scale above 100%). A saved choice stays visible.
    const bool fsr_card = fsr && !dev.integrated();
    std::vector<int> idx{0};
    if (rtx) idx.insert(idx.end(), {1, 2, 3, 4});
    if (fsr_card) idx.insert(idx.end(), {5, 6, 7});
    else if (fsr && wwe2k14::gfxs::IsFsr(s.upscale)) idx.push_back(s.upscale == 5 ? 5 : s.upscale == 6 ? 6 : 7);
    std::vector<const char*> names;
    int ui = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
      names.push_back(kUp[idx[i]]);
      if (kUpValue[idx[i]] == s.upscale) ui = int(i);
    }
    if (idx.size() > 1) {
      std::string note = kUpNote[idx[size_t(ui)]];
      if (!rtx && ui == 0)
        note += " DLSS needs an NVIDIA RTX card (found: " + (hw_.gpu.empty() ? std::string("unknown") : hw_.gpu) + ").";
      if (wwe2k14::gfxs::IsFsr(s.upscale) && dev.integrated())
        note += " Not offered on integrated graphics (" + dev.name + "): FSR does not support it.";
      if (OptionRow("UPSCALING", ui, names.data(), int(names.size()), true, note.c_str())) {
        s.upscale = kUpValue[idx[size_t(ui)]];
        s.fsr_note.clear();  // a new choice: the old note no longer applies
      }
    } else {
      int off = 0;
      const std::string why = "DLSS needs an NVIDIA RTX card (found: " + (hw_.gpu.empty() ? std::string("unknown") : hw_.gpu) +
                              (!fsr ? "); FSR needs amd_fidelityfx_vk.dll next to the game (missing)."
                                    : "); FSR is not offered on integrated graphics (" + dev.name + ").");
      OptionRow("UPSCALING", off, names.data(), 1, false, why.c_str());
    }
    if (!s.fsr_note.empty()) {  // Session 37: the game turned FSR off on this PC (src\upscale_guard.h)
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * d);
      ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
      ImGui::TextColored(kWarn, "Last time FSR could not run on this graphics card, so the game turned it off: %s",
                         s.fsr_note.c_str());
      ImGui::PopTextWrapPos();
    }
    if (rtx && wwe2k14::gfxs::IsNvidia(s.upscale)) {
      // Session 31 (Jarrod: the bundled files should just work): with NVIDIA's files next to the launcher (a release)
      // or in the project's Streamline SDK (a development build), nothing to set up; another folder is optional.
      const bool own = HasStreamline(s.dlss_dir), included = !paths_.nvidia.empty();
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * d);
      if (included && !own && !nv_other_) {
        if (paths_.nvidia_bundled) ImGui::TextColored(kGood, "Using the NVIDIA DLSS files included with this build.");
        else ImGui::TextColored(kGood, "Using NVIDIA's DLSS files from the project's Streamline SDK (development build).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Use other files...")) nv_other_ = true;
        if (!s.dlss_dir.empty())
          ImGui::TextDisabled("    Your folder %s has no NVIDIA files, so these are used.", s.dlss_dir.c_str());
      } else {
        ImGui::SetNextItemWidth(-110 * d);
        ImGui::InputTextWithHint("##sl", "folder with sl.interposer.dll (NVIDIA's Streamline runtime)", &s.dlss_dir);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##sl", ImVec2(-1, 0)))
          if (auto f = PickFolder(hwnd_, L"Pick the folder with sl.interposer.dll")) s.dlss_dir = PathStr(*f);
        if (HasStreamline(s.dlss_dir)) ImGui::TextColored(kGood, "Using the NVIDIA files in this folder.");
        else if (included) ImGui::TextColored(kWarn, "No NVIDIA files in this folder: the included ones are used.");
        else {
          ImGui::TextColored(kWarn, "NVIDIA runtime folder not set.");
          Note("Where to get these files: DLSS needs NVIDIA's Streamline runtime, a free download from NVIDIA's "
               "developer site (the Streamline SDK; use its bin\\x64 folder).");
        }
        if (included && ImGui::SmallButton("Back to the included files")) s.dlss_dir.clear(), nv_other_ = false;
      }
      ImGui::Dummy(ImVec2(0, 6 * d));
    }
    // Session 33: anisotropic filtering (the game always had 4x forced by the runtime; 8x / 16x sharpen floors,
    // ramps and mats seen at an angle). Changes at once from the game's own GRAPHICS screen too.
    static constexpr const char* kTf[] = {"STANDARD", "8X", "16X"};
    static constexpr const char* kTfNote[] = {"4x anisotropic filtering, as before.",
                                              "8x anisotropic filtering: sharper floors and ramps at an angle.",
                                              "16x anisotropic filtering: the sharpest floors and ramps at an angle."};
    s.texture_filter = std::clamp(s.texture_filter, 0, 2);
    OptionRow("TEXTURE FILTERING", s.texture_filter, kTf, 3, true, kTfNote[s.texture_filter]);
    OptionToggle("ORIGINAL LOOK", s.original_look, true,
                 "The Xbox 360 picture: 720p, the game's 4x MSAA, 30 fps entrances.");
  }

  // ---------------- About (Session 31: its own page; was a section of Settings). The project, its links, credits
  // (the NVIDIA DLSS licence asks for its credit here) and the licences file.
  static constexpr const char* kProjectUrl = "https://github.com/Jarrodfeng/wwe2k14-recomp";
  struct Credit {
    const char* name;
    const char* what;
    const char* url;
  };
  static constexpr Credit kCredits[] = {
      {"ReXGlue SDK", "the recompiler and runtime this is built on (Tom Clay)", "https://github.com/rexglue/rexglue-sdk"},
      {"Xenia", "the Xbox 360 research ReXGlue's runtime grew from (Ben Vanik and contributors)", "https://xenia.jp"},
      {"360tools", "stub templates and the recompilation workflow (sp00nznet)", "https://github.com/sp00nznet/360tools"},
      {"Dear ImGui", "this launcher's interface (Omar Cornut)", "https://github.com/ocornut/imgui"},
      {"SDL", "controllers, sound and the game window", "https://libsdl.org"},
      {"FFmpeg", "audio decoding", "https://ffmpeg.org"},
      {"stb_image", "pictures (Sean Barrett)", "https://github.com/nothings/stb"},
      {"nlohmann/json", "settings files (Niels Lohmann)", "https://github.com/nlohmann/json"},
      {"NVIDIA DLSS", "upscaling (NVIDIA Streamline)", "https://developer.nvidia.com/rtx/streamline"},
      {"AMD FidelityFX", "FSR 3 upscaling (AMD FidelityFX SDK)", "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK"},
      {"SteamGridDB", "the fan-made icon art", "https://www.steamgriddb.com"},
      // Session 67: CC BY-SA 3.0 asks for the credit, the licence's link and the changes made.
      {"360 controller.svg", "the KEYBOARD CONTROLS pad, by Alphathon (Wikimedia Commons), CC BY-SA 3.0; labels removed",
       "https://commons.wikimedia.org/wiki/File:360_controller.svg"},
  };

  void AboutPage() {
    const float d = Dpi();
    Heading("ABOUT", 1.6f);
    ImGui::Spacing();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 logo = DrawLogo(ImGui::GetWindowDrawList(), at, 300 * d, 1);
    if (logo.x > 0) {
      ImGui::Dummy(logo);
      ImGui::SameLine(0, 24 * d);
    }
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, logo.y * 0.18f));
    Heading("NATIVE PC RECOMPILATION", 1.15f);
    ImGui::Text("%s", Version());
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
    Note("WWE 2K14 (Xbox 360, 2013; Yuke's and Visual Concepts, published by 2K Sports) recompiled into a native "
         "Windows program. A non-commercial fan preservation project, not affiliated with WWE, 2K, Take-Two, Yuke's "
         "or Visual Concepts. No game files are included: it runs from your own copy of the disc.");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 10 * d));
    Heading("LINKS", 1.15f);
    ImGui::TextLinkOpenURL("The project on GitHub", kProjectUrl);
    ImGui::SameLine(0, 24 * d);
    if (ImGui::TextLink("Third-party licences")) OpenNotices();
    ImGui::SameLine(0, 24 * d);
    if (ImGui::TextLink("Report a problem")) ImGui::OpenPopup("report");
    ReportPopup();

    ImGui::Dummy(ImVec2(0, 10 * d));
    Heading("CREDITS", 1.15f);
    if (ImGui::BeginTable("credits", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 150 * d);
      ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("link", ImGuiTableColumnFlags_WidthFixed, 70 * d);
      for (const Credit& c : kCredits) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(c.name);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", c.what);
        ImGui::TableNextColumn();
        ImGui::PushID(c.name);
        ImGui::TextLinkOpenURL("website", c.url);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
    Note("NVIDIA, the NVIDIA logo, DLSS and RTX are trademarks and/or registered trademarks of NVIDIA Corporation. "
         "AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. "
         "The full list of libraries and their licences is in THIRD_PARTY_NOTICES.md next to the launcher.");
    ImGui::PopTextWrapPos();
  }

  // THIRD_PARTY_NOTICES.md next to the launcher (a release), else the repo's copy (a development build).
  void OpenNotices() {
    std::error_code ec;
    fs::path f = paths_.launcher_dir / "THIRD_PARTY_NOTICES.md";
    if (!fs::exists(f, ec) && paths_.repo_found) f = paths_.repo / "docs" / "release" / "THIRD_PARTY_NOTICES.md";
    if (!fs::exists(f, ec)) return Status("THIRD_PARTY_NOTICES.md not found next to the launcher");
    ShellExecuteW(hwnd_, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }

  void AdvancedSettings() {
    const float dpi = Dpi();
    {  // Session 67 (Jarrod): renamed from KEYBOARD AS CONTROLLER, with MAP KEYS beside it (the KEYBOARD CONTROLS window)
      const float bw = 150 * dpi, row_h = 38 * dpi;
      const ImVec2 row = ImGui::GetCursorScreenPos();
      OptionToggle("KEYBOARD CONTROLS", s.keyboard, true, "Play with the keyboard; MAP KEYS sets the key for each button.",
                   ImGui::GetContentRegionAvail().x - bw - 10 * dpi);
      const ImVec2 after = ImGui::GetCursorScreenPos();
      const float bx = row.x + ImGui::GetContentRegionAvail().x - bw;
      if (SlantButton("MAP KEYS", ImVec2(bx, row.y + 2 * dpi), ImVec2(bw, row_h - 4 * dpi), true, 17 * dpi))
        want_keys_popup_ = true;
      ImGui::SetCursorScreenPos(after);
    }
    // Session 27: the logo import's Session 25 route (counted presses, no screen checks), a fallback.
    OptionToggle("LOGO IMPORT: OLD ROUTE", s.logo_legacy_route, true, "Counted presses, no screen checks (a fallback).");
    ImGui::Dummy(ImVec2(0, 6 * dpi));
    ImGui::SetNextItemWidth(-200 * dpi);
    ImGui::InputTextWithHint("Extra flags", "--name=value ...", &s.extra);
    bool changed = false;
    ImGui::SetNextItemWidth(-200 * dpi);
    changed |= ImGui::InputTextWithHint("Save folder", PathStr(DefaultUserRoot()).c_str(), &s.user_data_dir);
    ImGui::SetNextItemWidth(-200 * dpi);
    changed |= ImGui::InputTextWithHint("Mods folder", PathStr(paths_.mods).c_str(), &s.mods_dir);
    ImGui::SetNextItemWidth(-200 * dpi);
    changed |= ImGui::InputTextWithHint("Game exe", PathStr(paths_.launcher_dir / "wwe2k14.exe").c_str(), &s.game_exe);
    if (changed) Reload();
    if (ImGui::Button("Show the first-run wizard again")) wizard_step_ = 0, want_wizard_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload everything")) Reload();
    ImGui::Spacing();
    ImGui::TextDisabled("Settings file: %s", PathStr(settings_path).c_str());
    auto row = [](const char* what, const fs::path& p) {
      std::error_code ec;
      ImGui::TextDisabled("%-12s %s %s", what, PathStr(p).c_str(), fs::exists(p, ec) ? "" : "(missing)");
    };
    row("Game data", paths_.game_data);
    row("Saves", paths_.user_data);
    row("Mods", paths_.mods);
    row("Logs", paths_.logs);
    row("Scenarios", paths_.scenarios);
    if (paths_.repo_found) row("Repo", paths_.repo);
    ImGui::TextDisabled("%zu characters, %zu arenas, %zu saved matches", chars_.size(), arenas_.size(), scenarios_.size());
  }

  // ---------------- Session 67: KEYBOARD CONTROLS (Jarrod: "a little button next to the keyboard as controller ... to
  // actually map the buttons ... a little diagram of a 360 controller"). An Xbox 360 pad drawn in the middle, each control
  // joined by a line to its keys on the left or right (the game's own controls screens). A key chip: click = press the
  // new key (Esc cancels; a mouse button clicked on the chip binds that button), right-click = remove it, "+" = add an
  // alternative. Clicking a control on the pad changes its first key. Each change is saved to <save folder>\keybinds.toml
  // at once (keyboard_binds.h), which the game reads when it starts.
  keys::BindFile keys_;
  fs::path keys_file_;
  int key_hot_ = -1, key_hot_force_ = -1;  // the control lit on the pad (a hovered row or control; --keys-hot)
  int cap_bind_ = -1, cap_slot_ = 0, cap_frame_ = 0;
  ImVec2 cap_min_{}, cap_max_{};  // the chip waiting for a key
  std::string keys_status_;
  bool keys_status_warn_ = false, want_keys_popup_ = false;

  // The callout groups: a heading with its controls' rows, joined to the control at `at` (pad units, below).
  struct KeyGroup {
    const char* title;
    int first, count;
    ImVec2 at;
    float r;  // the line stops this far short of `at` (the control's edge: it does not cross the button)
  };
  // (`at` and `r` in the pad picture's units: the SVG's, kPadBox below)
  static constexpr KeyGroup kLeftGroups[] = {{"LT", 4, 1, {665, 170}, 0},       {"LB", 6, 1, {620, 272}, 0},
                                             {"BACK", 22, 1, {875, 477}, 36},    {"LEFT STICK", 8, 5, {650, 520}, 80},
                                             {"D-PAD", 18, 4, {824, 650}, 114}};
  static constexpr KeyGroup kRightGroups[] = {{"RT", 5, 1, {1335, 170}, 0},     {"RB", 7, 1, {1380, 272}, 0},
                                              {"GUIDE", 24, 1, {1002, 476}, 72}, {"Y", 3, 1, {1360, 394}, 44},
                                              {"START", 23, 1, {1131, 477}, 36}, {"B", 1, 1, {1448, 462}, 44},
                                              {"X", 2, 1, {1258, 472}, 44},      {"A", 0, 1, {1353, 541}, 46},
                                              {"RIGHT STICK", 13, 5, {1163, 686}, 80}};

  // "LEFT STICK UP", "A", ... (for the messages).
  static std::string BindTitle(int b) {
    for (const KeyGroup& g : kLeftGroups)
      if (b >= g.first && b < g.first + g.count) return g.count > 1 ? std::string(g.title) + " " + keys::kBinds[b].label : g.title;
    for (const KeyGroup& g : kRightGroups)
      if (b >= g.first && b < g.first + g.count) return g.count > 1 ? std::string(g.title) + " " + keys::kBinds[b].label : g.title;
    return keys::kBinds[b].label;
  }

  void LoadKeys() {
    keys_file_ = paths_.user_data / "keybinds.toml";
    keys_ = keys::LoadBindFile(keys_file_);
  }
  void SaveKeys(const std::string& what) {
    if (keys::SaveBindFile(keys_file_, keys_)) {
      if (!what.empty()) keys_status_ = what, keys_status_warn_ = false;
    } else {
      keys_status_ = "Could not write " + PathStr(keys_file_), keys_status_warn_ = true;
    }
  }

  void BeginCapture(int bind, int slot) {
    if (keys_file_.empty()) LoadKeys();
    cap_bind_ = bind, cap_slot_ = slot, cap_frame_ = ImGui::GetFrameCount();
    g_key_capture = {};
    g_key_capture.active = true;
    keys_status_ = "Press the key for " + BindTitle(bind) + " (Esc: cancel; or click the box with a mouse button).";
    keys_status_warn_ = false;
  }
  void EndCapture() {
    cap_bind_ = -1;
    g_key_capture = {};
  }
  // The captured key (an SDK name with its modifiers) into the waiting slot.
  void ApplyKey(const std::string& token) {
    const int b = cap_bind_;
    EndCapture();
    if (b < 0) return;
    std::vector<std::string> alts = keys::Split(keys::Value(keys_, b));
    const size_t placed = std::min(size_t(std::max(cap_slot_, 0)), alts.size());  // that chip, or a new last one
    if (placed < alts.size()) alts[placed] = token;
    else alts.push_back(token);
    for (size_t i = alts.size(); i-- > 0;)  // the same key once per control (the one just placed stays)
      if (i != placed && alts[i] == token) alts.erase(alts.begin() + std::ptrdiff_t(i));
    keys::SetBind(keys_, b, keys::Join(alts));
    std::string also;
    for (int i = 0; i < keys::kBindCount; ++i)
      if (i != b)
        for (const auto& a : keys::Split(keys::Value(keys_, i)))
          if (a == token) also += (also.empty() ? "" : ", ") + BindTitle(i);
    SaveKeys(BindTitle(b) + " = " + keys::Display(token) + ". Saved.");
    if (!also.empty()) {
      keys_status_ = keys::Display(token) + " is also " + also + ": it presses both.", keys_status_warn_ = true;
    } else if (token.size() >= 2 && token[0] == 'F' && std::isdigit(static_cast<unsigned char>(token[1]))) {
      keys_status_ += " (The game's own tools use some F-keys too: F4 opens its settings.)";
    }
  }

  // The pad picture (Jarrod's 360_controller.svg: tools\launcher\make_pad_png.py) covers kPadBox of the SVG's units;
  // positions below are in those units, scaled by `k` from the picture's top-left `o`.
  static constexpr float kPadBox[4] = {415, 100, 1170, 725};  // x, y, w, h (make_pad_png.py VIEW)
  static ImVec2 PadPt(ImVec2 o, float k, float x, float y) {
    return ImVec2(o.x + (x - kPadBox[0]) * k, o.y + (y - kPadBox[1]) * k);
  }

  // The pad part a bind belongs to (its colour in the parts picture, make_pad_png.py PARTS): 0-3 A B X Y, 4 / 5 LT / RT,
  // 6 / 7 LB / RB, 8 left stick, 9 right stick, 10 D-pad, 11 BACK, 12 START, 13 GUIDE.
  static int PartOf(int b) {
    if (b <= 7) return b;
    if (b <= 12) return 8;
    if (b <= 17) return 9;
    if (b <= 21) return 10;
    return b - 22 + 11;
  }
  static constexpr int kPadParts = 14;
  // The sticks' and the D-pad's centres and radii (for the direction arrows and which direction a click is).
  static constexpr float kLeftStick[3] = {650, 520, 76}, kRightStick[3] = {1163, 686, 76}, kDpad[3] = {824, 650, 112};

  // Resources 102 (the pad, 2x) and 103 (the parts picture, 1x), decoded once; textures per drawn width.
  bool pad_tried_ = false;
  std::vector<uint8_t> pad_px_, parts_px_;
  int pad_w_ = 0, pad_h_ = 0, parts_w_ = 0, parts_h_ = 0;
  ID3D11ShaderResourceView* pad_srv_ = nullptr;
  int pad_srv_w_ = 0;
  std::map<std::pair<int, int>, ID3D11ShaderResourceView*> part_srv_;  // (part, drawn width) -> its tint mask

  static std::vector<uint8_t> LoadPngResource(int id, int& w, int& h) {
    const HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const auto* data = static_cast<const stbi_uc*>(g ? LockResource(g) : nullptr);
    int n = 0;
    std::vector<uint8_t> out;
    if (data)
      if (stbi_uc* px = stbi_load_from_memory(data, int(SizeofResource(nullptr, r)), &w, &h, &n, 4)) {
        out.assign(px, px + size_t(w) * size_t(h) * 4);
        stbi_image_free(px);
      }
    return out;
  }

  // The part at a pixel of the parts picture (-1 = none): R = 10 + 17 * part, G 200, B 100 (make_pad_png.py).
  int PartAtPixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= parts_w_ || y >= parts_h_) return -1;
    const uint8_t* p = &parts_px_[(size_t(y) * size_t(parts_w_) + size_t(x)) * 4];
    if (p[3] != 255 || p[1] != 200 || p[2] != 100 || p[0] < 10 || (p[0] - 10) % 17) return -1;
    const int part = (p[0] - 10) / 17;
    return part < kPadParts ? part : -1;
  }

  void DrawPad(ImDrawList* dl, ImVec2 o, float k, int hot) {
    if (!pad_tried_) {
      pad_tried_ = true;
      pad_px_ = LoadPngResource(102, pad_w_, pad_h_);
      parts_px_ = LoadPngResource(103, parts_w_, parts_h_);
    }
    const float d = Dpi();
    const int w = std::clamp(int(std::lround(kPadBox[2] * k)), 16, 4096);  // (KeysPopup keeps it to steps of 8 px)
    const int h = std::max(1, int(std::lround(double(w) * kPadBox[3] / kPadBox[2])));
    const ImVec2 hi(o.x + float(w), o.y + float(h));
    if (pad_px_.empty()) {
      dl->AddRect(o, hi, IM_COL32(120, 120, 128, 255));
      dl->AddText(ImVec2(o.x + 12 * d, o.y + 12 * d), IM_COL32(170, 170, 178, 255), "(the controller picture is missing)");
      return;
    }
    if (!pad_srv_ || pad_srv_w_ != w) {
      ReleaseLater(pad_srv_);
      for (auto& [key, srv] : part_srv_) ReleaseLater(srv);  // (the masks of the old size)
      part_srv_.clear();
      pad_srv_ = MakeTexture(theme::Resample(pad_px_.data(), pad_w_, pad_h_, w, h), w, h);
      pad_srv_w_ = w;
    }
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(pad_srv_)), o, hi);
    if (hot < 0 || parts_px_.empty()) return;
    // The lit control: its own shape tinted red (a white mask of its pixels, scaled to the drawn size).
    const int part = PartOf(hot);
    ID3D11ShaderResourceView*& mask = part_srv_[{part, w}];
    if (!mask) {
      std::vector<uint8_t> m(size_t(parts_w_) * size_t(parts_h_) * 4, 255);
      for (int y = 0; y < parts_h_; ++y)
        for (int x = 0; x < parts_w_; ++x) m[(size_t(y) * size_t(parts_w_) + size_t(x)) * 4 + 3] = PartAtPixel(x, y) == part ? 255 : 0;
      mask = MakeTexture(theme::Resample(m.data(), parts_w_, parts_h_, w, h), w, h);
    }
    const bool stick = part == 8 || part == 9, dpad = part == 10;
    const int first = part == 8 ? 8 : part == 9 ? 13 : 18;
    const bool direction = (stick || dpad) && hot - first < 4;
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(mask)), o, hi, ImVec2(0, 0), ImVec2(1, 1),
                 direction ? IM_COL32(214, 22, 30, 110) : IM_COL32(214, 22, 30, 170));
    if (direction) {  // a red arrow outside the stick or the D-pad, pointing the lit way
      const float* c = part == 8 ? kLeftStick : part == 9 ? kRightStick : kDpad;
      const int dir = hot - first;  // 0 up, 1 down, 2 left, 3 right
      const ImVec2 v = dir == 0 ? ImVec2(0, -1) : dir == 1 ? ImVec2(0, 1) : dir == 2 ? ImVec2(-1, 0) : ImVec2(1, 0);
      const ImVec2 n(-v.y, v.x), at = PadPt(o, k, c[0], c[1]);
      const float r0 = (c[2] + 8) * k, r1 = (c[2] + 62) * k, half = 30 * k;
      const ImVec2 tip(at.x + v.x * r1, at.y + v.y * r1), base(at.x + v.x * r0, at.y + v.y * r0);
      dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * half, base.y + n.y * half),
                            ImVec2(base.x - n.x * half, base.y - n.y * half), kMenuRed);
      dl->AddTriangle(tip, ImVec2(base.x + n.x * half, base.y + n.y * half),
                      ImVec2(base.x - n.x * half, base.y - n.y * half), IM_COL32(0, 0, 0, 200), std::max(1.f, d));
    }
  }

  // The bind under the mouse on the pad, or -1: the control from the parts picture; a stick's or the D-pad's direction
  // by where on it (a stick's middle = PRESS).
  int PadHit(ImVec2 m, ImVec2 o, float k) const {
    if (parts_px_.empty() || k <= 0) return -1;
    const float sx = float(parts_w_) / (kPadBox[2] * k), sy = float(parts_h_) / (kPadBox[3] * k);
    const int part = PartAtPixel(int((m.x - o.x) * sx), int((m.y - o.y) * sy));
    if (part < 0) return -1;
    if (part <= 7) return part;
    if (part >= 11) return 22 + part - 11;
    const float* c = part == 8 ? kLeftStick : part == 9 ? kRightStick : kDpad;
    const int first = part == 8 ? 8 : part == 9 ? 13 : 18;
    const ImVec2 at = PadPt(o, k, c[0], c[1]);
    const float dx = m.x - at.x, dy = m.y - at.y;
    if (part != 10 && dx * dx + dy * dy < (c[2] * 0.45f * k) * (c[2] * 0.45f * k)) return first + 4;
    if (std::fabs(dy) >= std::fabs(dx)) return dy < 0 ? first : first + 1;
    return dx < 0 ? first + 2 : first + 3;
  }

  // One key chip at `pos`: a small key cap with the key's label. Returns 1 when clicked, 2 when right-clicked.
  int KeyChip(const char* id, const std::string& text, ImVec2 pos, float h, bool waiting, bool faint) {
    const float d = Dpi(), pad = 8 * d;
    const std::string shown = waiting ? "PRESS A KEY" : text;
    const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    const ImVec2 size(std::max(ts.x + pad * 2, h * 1.15f), h);
    ImGui::SetCursorScreenPos(pos);
    int r = 0;
    bool hover = false;
    if (waiting) {
      ImGui::Dummy(size);  // no button: a click on it is the mouse button to bind (KeysPopup)
      cap_min_ = pos, cap_max_ = ImVec2(pos.x + size.x, pos.y + size.y);
    } else {
      ImGui::PushID(id);
      if (ImGui::InvisibleButton("##chip", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                     ImGuiButtonFlags_PressedOnClick))
        r = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 2 : 1;
      hover = ImGui::IsItemHovered();
      if (hover && !faint) ImGui::SetItemTooltip("Click to change, right-click to remove");
      ImGui::PopID();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 hi(pos.x + size.x, pos.y + size.y);
    const float pulse = waiting ? 0.6f + 0.4f * std::sin(float(ImGui::GetTime()) * 6.f) : 1.f;
    const ImU32 fill = waiting ? IM_COL32(int(150 * pulse), 18, 26, 255) : faint ? IM_COL32(40, 41, 47, 150)
                                                                                 : IM_COL32(48, 49, 56, 255);
    dl->AddRectFilled(pos, hi, fill, 3 * d);
    dl->AddRect(pos, hi, waiting || hover ? kMenuRed : faint ? IM_COL32(110, 110, 118, 150) : IM_COL32(150, 150, 158, 255),
                3 * d, 0, std::max(1.f, (hover ? 1.6f : 1.f) * d));
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + (h - ts.y) / 2),
                faint ? IM_COL32(170, 170, 178, 255) : IM_COL32_WHITE, shown.c_str());
    ImGui::SetCursorScreenPos(ImVec2(hi.x + 5 * d, pos.y));
    return r;
  }

  // One column of callouts at `x0` (`width` wide). Returns where each group's line starts (x, y) and bends (z = x)
  // through `anchors`: on the left after the heading row's text or keys, then out to the column's edge.
  void KeyColumn(const KeyGroup* groups, int n, float x0, float y0, float width, float rh, float gap, bool right,
                 std::vector<ImVec4>& anchors, int& hovered) {
    const float d = Dpi(), chips_x = x0 + 78 * d, ch = rh - 5 * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = y0;
    for (int gi = 0; gi < n; ++gi) {
      const KeyGroup& g = groups[gi];
      const bool group_hot = key_hot_ >= g.first && key_hot_ < g.first + g.count;
      float head_end = x0 + 4 * d + MenuTextSize(17 * d, g.title).x;  // (moved past a one-row control's keys below)
      const int rows = g.count > 1 ? g.count + 1 : 1;
      for (int r = 0; r < rows; ++r) {
        const int b = g.count > 1 ? (r == 0 ? -1 : g.first + r - 1) : g.first;
        const ImVec2 lo(x0, y), hi(x0 + width, y + rh);
        const bool row_hover = b >= 0 && ImGui::IsMouseHoveringRect(lo, hi) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        if (row_hover) hovered = b;
        if (b >= 0 && (key_hot_ == b || cap_bind_ == b)) {
          const float sk = 6 * d;
          const ImVec2 q[4] = {{lo.x + sk, lo.y}, {hi.x, lo.y}, {hi.x - sk, hi.y}, {lo.x, hi.y}};
          dl->AddConvexPolyFilled(q, 4, IM_COL32(0, 0, 0, 130));
        }
        if (r == 0) {  // the heading: the control's name (and its keys for a one-row control)
          const float px = 17 * d;
          MenuText(dl, ImVec2(x0 + 4 * d, y + (rh - px) / 2), px, group_hot ? kMenuRed : IM_COL32_WHITE, g.title);
        } else {
          const char* label = keys::kBinds[b].label;
          dl->AddText(ImVec2(x0 + 18 * d, y + (rh - ImGui::GetTextLineHeight()) / 2),
                      key_hot_ == b ? IM_COL32(240, 120, 120, 255) : IM_COL32(160, 161, 168, 255), label);
        }
        if (b >= 0) {
          const std::vector<std::string> alts = keys::Split(keys::Value(keys_, b));
          ImVec2 at(chips_x, y + 2.5f * d);
          for (int a = 0; a < int(alts.size()); ++a) {
            const std::string id = std::to_string(b) + "_" + std::to_string(a);
            const int click = KeyChip(id.c_str(), keys::Display(alts[size_t(a)]), at, ch, cap_bind_ == b && cap_slot_ == a, false);
            at = ImGui::GetCursorScreenPos();
            if (click == 1) BeginCapture(b, a);
            if (click == 2) {
              std::vector<std::string> left = alts;
              left.erase(left.begin() + a);
              keys::SetBind(keys_, b, keys::Join(left));
              EndCapture();
              SaveKeys(BindTitle(b) + ": " + keys::Display(alts[size_t(a)]) + " removed. Saved.");
            }
          }
          if (r == 0 && !alts.empty()) head_end = at.x - 5 * d;  // (not past the "+" a hover shows: the line stays put)
          const bool add_waiting = cap_bind_ == b && cap_slot_ >= int(alts.size());
          if (int(alts.size()) < keys::kMaxAlternatives && (row_hover || alts.empty() || add_waiting)) {
            const std::string id = std::to_string(b) + "_add";
            if (KeyChip(id.c_str(), alts.empty() ? "+ ADD" : "+", at, ch, add_waiting, true) == 1)
              BeginCapture(b, int(alts.size()));
            if (r == 0 && alts.empty()) head_end = ImGui::GetCursorScreenPos().x - 5 * d;
          }
        }
        if (r == 0)
          anchors.push_back(right ? ImVec4(x0 - 6 * d, y + rh / 2, x0 - 18 * d, 0)
                                  : ImVec4(head_end + 6 * d, y + rh / 2, std::max(head_end + 6 * d, x0 + width + 6 * d), 0));
        y += rh;
      }
      y += gap;
    }
  }

  static float KeyColumnHeight(const KeyGroup* groups, int n, float rh, float gap) {
    float h = 0;
    for (int i = 0; i < n; ++i) h += (groups[i].count > 1 ? groups[i].count + 1 : 1) * rh + gap;
    return h - gap;
  }

  void KeysPopup() {
    if (want_keys_popup_) {
      want_keys_popup_ = false;
      LoadKeys();
      keys_status_ = keys_.exists ? "" : "No keys changed yet: these are the defaults.", keys_status_warn_ = false;
      ImGui::OpenPopup("keys");
    }
    const float d = Dpi();
    if (!BeginGameModal("keys", "KEYBOARD CONTROLS", 1060 * d)) {
      if (cap_bind_ >= 0) EndCapture();
      return;
    }
    // A key pressed while a chip waits (WndProc took it).
    if (cap_bind_ >= 0 && g_key_capture.got) {
      const KeyCapture c = g_key_capture;
      if (c.vk == VK_ESCAPE && !c.shift && !c.ctrl && !c.alt) {
        EndCapture();
        keys_status_ = "Cancelled.", keys_status_warn_ = false;
      } else if (const std::string name = keys::NameFromKeyDown(c.vk, c.lp); name.empty()) {
        EndCapture();
        keys_status_ = "That key can't be used by the game: try another.", keys_status_warn_ = true;
      } else {
        ApplyKey(keys::WithModifiers(name, c.shift, c.ctrl, c.alt));
      }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = ImGui::GetContentRegionAvail().x, gap = 8 * d, col_w = 235 * d;
    // Row height: 25 px, less when the window is short (the rest of the window needs ~300 px).
    const float rows_room = vp->WorkSize.y - 330 * d;
    const float rh = std::clamp(rows_room / 15.f, 19 * d, 25 * d);
    const float col_h = std::max(KeyColumnHeight(kLeftGroups, int(std::size(kLeftGroups)), rh, gap),
                                 KeyColumnHeight(kRightGroups, int(std::size(kRightGroups)), rh, gap));
    const float pad_w = width - 2 * col_w - 40 * d;
    // The pad's scale: as large as fits, its drawn width in steps of 8 px (one picture per step while resizing).
    const float fit = std::max(0.05f, std::min(pad_w / kPadBox[2], col_h / kPadBox[3]));
    const float k = float(std::max(16, int(kPadBox[2] * fit) / 8 * 8)) / kPadBox[2];
    const float pw = kPadBox[2] * k, ph = kPadBox[3] * k;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float canvas_h = std::max(col_h, ph);
    const ImVec2 pad_o(std::floor(top.x + col_w + 20 * d + (pad_w - pw) / 2), std::floor(top.y + (canvas_h - ph) / 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The pad first (clicks on a control: change its first key), then the two columns over the lines.
    ImGui::SetCursorScreenPos(ImVec2(pad_o.x, pad_o.y));
    ImGui::InvisibleButton("##pad", ImVec2(pw, ph), ImGuiButtonFlags_PressedOnClick);
    const bool pad_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const int pad_hover = ImGui::IsItemHovered() ? PadHit(ImGui::GetIO().MousePos, pad_o, k) : -1;
    if (pad_clicked && pad_hover >= 0) BeginCapture(pad_hover, 0);
    if (pad_hover >= 0) ImGui::SetTooltip("%s: click to change its key", BindTitle(pad_hover).c_str());
    DrawPad(dl, pad_o, k, key_hot_);
    int row_hover = -1;
    std::vector<ImVec4> left_anchor, right_anchor;
    const float ly = top.y + (canvas_h - KeyColumnHeight(kLeftGroups, int(std::size(kLeftGroups)), rh, gap)) / 2;
    const float ry = top.y + (canvas_h - KeyColumnHeight(kRightGroups, int(std::size(kRightGroups)), rh, gap)) / 2;
    KeyColumn(kLeftGroups, int(std::size(kLeftGroups)), top.x, ly, col_w, rh, gap, false, left_anchor, row_hover);
    KeyColumn(kRightGroups, int(std::size(kRightGroups)), top.x + width - col_w, ry, col_w, rh, gap, true, right_anchor,
              row_hover);
    // The lines: from each heading out to its control (red for the lit one).
    auto lines = [&](const KeyGroup* groups, const std::vector<ImVec4>& anchors) {
      for (size_t i = 0; i < anchors.size(); ++i) {
        const KeyGroup& g = groups[i];
        const bool hot = key_hot_ >= g.first && key_hot_ < g.first + g.count;
        const ImU32 col = hot ? kMenuRed : IM_COL32(150, 151, 158, 255);
        const ImVec2 a(anchors[i].x, anchors[i].y), elbow(anchors[i].z, anchors[i].y), c = PadPt(pad_o, k, g.at.x, g.at.y);
        const float dx = elbow.x - c.x, dy = elbow.y - c.y, len = std::max(1.f, std::sqrt(dx * dx + dy * dy));
        const ImVec2 t(c.x + dx / len * g.r * k, c.y + dy / len * g.r * k);
        const float lw = hot ? 2.f * d : 1.2f * d;
        dl->AddLine(a, elbow, col, lw);
        dl->AddLine(elbow, t, IM_COL32(0, 0, 0, 150), lw + 2 * d);  // a dark edge: the line shows on the white pad too
        dl->AddLine(elbow, t, col, lw);
        dl->AddCircleFilled(t, (hot ? 4.f : 3.f) * d, IM_COL32(0, 0, 0, 150));
        dl->AddCircleFilled(t, (hot ? 3.f : 2.f) * d, col);
      }
    };
    lines(kLeftGroups, left_anchor);
    lines(kRightGroups, right_anchor);
    key_hot_ = key_hot_force_ >= 0 ? key_hot_force_ : cap_bind_ >= 0 ? cap_bind_ : row_hover >= 0 ? row_hover : pad_hover;
    ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + canvas_h + 10 * d));

    // A mouse click while a chip waits: on the chip = that mouse button, elsewhere = cancel.
    if (cap_bind_ >= 0 && cap_frame_ != ImGui::GetFrameCount()) {
      static constexpr const char* kMouse[3] = {"LMB", "RMB", "MMB"};
      for (int bt = 0; bt < 3 && cap_bind_ >= 0; ++bt) {
        if (!ImGui::IsMouseClicked(bt)) continue;
        if (ImGui::IsMouseHoveringRect(cap_min_, cap_max_)) {
          const ImGuiIO& io = ImGui::GetIO();
          ApplyKey(keys::WithModifiers(kMouse[bt], io.KeyShift, io.KeyCtrl, io.KeyAlt));
        } else {
          EndCapture();
          keys_status_ = "Cancelled.", keys_status_warn_ = false;
        }
      }
    }

    // The switch itself and the mouse.
    const float half = (width - 16 * d) / 2;
    OptionToggle("KEYBOARD CONTROLS", s.keyboard, true, nullptr, half);
    ImGui::SameLine(0, 16 * d);
    bool mouse = keys::MouseOn(keys_);
    if (OptionToggle("MOUSE AS RIGHT STICK", mouse, true, nullptr, half)) {
      keys::SetMouse(keys_, mouse);
      SaveKeys(mouse ? "The mouse moves the right stick. Saved." : "The mouse is off. Saved.");
    }
    static constexpr double kSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0};
    static constexpr const char* kSpeedNames[] = {"0.25x", "0.5x", "0.75x", "1x", "1.5x", "2x", "3x", "4x"};
    int si = 0;
    const double sens = keys::Sensitivity(keys_);
    for (int i = 0; i < int(std::size(kSpeeds)); ++i)
      if (std::fabs(kSpeeds[i] - sens) < std::fabs(kSpeeds[si] - sens)) si = i;
    if (OptionRow("MOUSE SPEED", si, kSpeedNames, int(std::size(kSpeedNames)), mouse, nullptr, half)) {
      keys::SetSensitivity(keys_, kSpeeds[si]);
      SaveKeys(std::string("Mouse speed ") + kSpeedNames[si] + ". Saved.");
    }
    ImGui::SameLine(0, 16 * d);
    {  // beside MOUSE SPEED: what the window says now
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(at.x + 6 * d, at.y + 2 * d));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + half - 10 * d);
      std::string note = keys_status_;
      if (note.empty()) note = "Click a key to change it, or a button on the pad. Saved to " + PathStr(keys_file_) + ".";
      if (game_proc_) note += " The game reads the keys when it starts: restart it to use changes.";
      if (!s.keyboard) note += " KEYBOARD CONTROLS is OFF: the game uses only controllers.";
      ImGui::PushStyleColor(ImGuiCol_Text, keys_status_warn_ ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("%s", note.c_str());
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, 6 * d));
    if (ModalMainButton("DONE")) EndCapture(), ImGui::CloseCurrentPopup();
    if (ModalButton("Reset to defaults")) {
      EndCapture();
      for (int i = 0; i < keys::kBindCount; ++i) keys::SetBind(keys_, i, keys::kBinds[i].def);
      keys::SetMouse(keys_, false);
      keys::SetSensitivity(keys_, 1.0);
      SaveKeys("Back to the default keys. Saved.");
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }

  // ---------------- First run
  // The first-run wizard (Session 25; Session 31: the game's look, and the last step's suggestions as rows you can
  // change, with the language and the graphics card).
  void Wizard() {
    if (want_wizard_) ImGui::OpenPopup("Welcome"), want_wizard_ = false;
    if (!BeginGameModal("Welcome", "WELCOME", 780 * Dpi())) return;
    const float d = Dpi();
    // The steps, like the game's tabs (not clickable: Back / Next move).
    {
      static constexpr const char* kSteps[] = {"1  GAME FILES", "2  YOUR SAVE", "3  SETTINGS"};
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float px = 18 * d, h = 30 * d, pad = 14 * d, sk = 8 * d;
      for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, 4 * d);
        const ImVec2 ts = MenuTextSize(px, kSteps[i]), pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(ts.x + pad * 2, h));
        if (i == wizard_step_) {
          const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + ts.x + pad * 2, pos.y},
                               {pos.x + ts.x + pad * 2 - sk, pos.y + h}, {pos.x, pos.y + h}};
          dl->AddConvexPolyFilled(q, 4, IM_COL32(0, 0, 0, 255));
          MenuTextRaw(dl, ImVec2(pos.x + pad, pos.y + (h - ts.y) / 2), px, kMenuRed, kSteps[i]);
        } else {
          MenuText(dl, ImVec2(pos.x + pad, pos.y + (h - ts.y) / 2), px, IM_COL32(141, 142, 147, 255), kSteps[i]);
        }
      }
    }
    ImGui::Dummy(ImVec2(0, 10 * d));
    if (wizard_step_ == 0) {
      ImGui::TextWrapped("This plays WWE 2K14 natively on your PC, from your own copy of the Xbox 360 disc. No game "
                         "files come with it: point it at the files you copied from your disc.");
      ImGui::Spacing();
      GameDataSection(true);
    } else if (wizard_step_ == 1) {
      RefreshLogos(true);
      if (logo_.save_exists) {
        ImGui::TextColored(kGood, "Found your save (%d logo slot(s) used).", kLogoSlots - logo_.Free());
        ImGui::TextDisabled("%s", PathStr(logo_.title_dir).c_str());
      } else {
        ImGui::TextWrapped("No save yet. That's fine: the game makes one the first time you play. (Mods -> Logos needs "
                           "it, so start the game once before importing logos.)");
        ImGui::TextDisabled("Saves go to %s", PathStr(paths_.user_data).c_str());
      }
    } else {
      if (!wiz_ready_) {  // the suggestions, once per visit of this step
        wiz_ = s, wiz_.resolution = SuggestResolution(hw_), wiz_.draw_height = 0, wiz_.frame_rate = SuggestFrameRate(hw_), wiz_.upscale = 0;
        SuggestForDevice(wiz_);  // Session 37: integrated graphics -> LIGHT at 720p
        wiz_ready_ = true;
      }
      ImGui::TextDisabled("Suggested for your PC (%s, %dx%d at %d Hz). You can change them later in Settings.",
                          hw_.gpu.c_str(), hw_.screen_w, hw_.screen_h, hw_.refresh_hz);
      ImGui::Dummy(ImVec2(0, 4 * d));
      int li = 0;
      for (int i = 0; i < int(std::size(kLanguageIds)); ++i)
        if (kLanguageIds[i] == wiz_.language) li = i;
      if (OptionRow("LANGUAGE", li, kLanguageNames, int(std::size(kLanguageNames)))) wiz_.language = kLanguageIds[li];
      if (gpus_.empty()) gpus_ = ListGpus();
      std::vector<std::string> names{"AUTOMATIC"};
      for (const Gpu& g : gpus_) names.push_back(g.name);
      std::vector<const char*> ptrs;
      for (const auto& n : names) ptrs.push_back(n.c_str());
      int gi = 0;
      for (size_t i = 0; i < gpus_.size(); ++i)
        if (gpus_[i].name == wiz_.gpu) gi = int(i) + 1;
      if (OptionRow("GRAPHICS CARD", gi, ptrs.data(), int(ptrs.size()), true,
                    gpus_.size() > 1 ? "Automatic: the dedicated card with the most memory." : nullptr)) {
        wiz_.gpu = gi == 0 ? std::string() : gpus_[size_t(gi - 1)].name;
        wiz_.resolution = SuggestResolution(hw_), wiz_.draw_height = 0, wiz_.frame_rate = SuggestFrameRate(hw_), wiz_.renderer = 2;
        SuggestForDevice(wiz_);  // Session 37: the suggestions follow the card
      }
      ResolutionRow(wiz_, true, ChosenDevice(wiz_.gpu).integrated() ? "540p suggested for integrated graphics." : nullptr);
      FrameRateRow(wiz_);
      ImGui::TextDisabled("%s", hw_.rtx ? "Upscaling (DLSS / DLAA / FSR) is in Settings -> Graphics."
                                        : "Upscaling (AMD FSR) is in Settings -> Graphics.");
    }
    ImGui::Dummy(ImVec2(0, 12 * d));
    if (wizard_step_ == 0) {
      if (ModalMainButton("NEXT", gd_.ok)) wizard_step_ = 1;
      if (ModalButton("Skip for now")) wizard_step_ = 1;
    } else if (wizard_step_ == 1) {
      if (ModalMainButton("NEXT")) wizard_step_ = 2, wiz_ready_ = false;
      if (ModalButton("Back")) wizard_step_ = 0;
    } else {
      if (ModalMainButton("FINISH")) {
        s.language = wiz_.language, s.gpu = wiz_.gpu, s.resolution = wiz_.resolution;
        s.frame_rate = wiz_.frame_rate, s.upscale = wiz_.upscale, s.renderer = wiz_.renderer;
        FinishWizard();
        ImGui::CloseCurrentPopup();
      }
      if (ModalButton("Back")) wizard_step_ = 1;
      if (ModalButton("Keep my settings")) FinishWizard(), ImGui::CloseCurrentPopup();
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }
  Settings wiz_;            // the wizard's last step: the suggestions, changed in place until FINISH
  bool wiz_ready_ = false;

  // (History.) Session 37 (the brief: "The first-run wizard should pick it on integrated graphics"): integrated graphics get the
  // light renderer at 720p (a UHD 620 drew 5-10 fps in matches with the accurate renderer at 720p); a card that cannot
  // run the accurate renderer (no interlock: older AMD drivers / graphics) gets the light renderer.
  // Session 41 (low-spec step 2): integrated graphics get the LOW preset (NATIVE, or LIGHT without interlock; 720p,
  // FRAME RATE 30). Session 51 ("NATIVE for everyone"): NATIVE on every card with interlock; integrated graphics still
  // start at 720p / 30 fps without an upscaler (the LOW preset's quality part, no button any more).
  // Session 54 (Jarrod, 2026-10-23: "540 as the default for igpus, as well as native as default for unsupported
  // interlock GPUs ... to make sure AMD people try the right thing"): NATIVE on cards without interlock too (it runs
  // there now: gpu_native_truth_host_rt), integrated graphics at 540p (NATIVE draws at 960x540).
  void SuggestForDevice(Settings& t) {
    const wwe2k14::vkdev::Device dev = ChosenDevice(t.gpu);
    if ((!dev.name.empty() && !dev.interlock) || dev.integrated()) t.renderer = 2;
    if (dev.integrated()) t.resolution = 0, t.draw_height = 540, t.frame_rate = 3, t.refresh_hz = 0, t.upscale = 0, t.original_look = false;
  }

  // Session 54: once for a file from before settings version 3 (after the first-run setup), the same two choices
  // where the file still has what the setup used to pick: LIGHT on a card without interlock -> NATIVE; NATIVE at 720p
  // on integrated graphics -> 540p. Logged; true when something moved.
  bool MigrateForDevice(Settings& t) {
    const wwe2k14::vkdev::Device dev = ChosenDevice(t.gpu);
    bool moved = false;
    if (!dev.name.empty() && !dev.interlock && t.renderer == 1) {
      t.renderer = 2, moved = true;
      Log("settings: no interlock on " + dev.name + ": renderer LIGHT -> NATIVE (once, Session 54)");
    }
    if (dev.integrated() && t.renderer == 2 && t.resolution == 0 && t.draw_height == 0 && t.upscale == 0) {
      t.draw_height = 540, moved = true;
      Log("settings: integrated graphics: resolution 720p -> 540p (once, Session 54)");
    }
    t.loaded_version = wwe2k14::gfxs::kSettingsVersion;
    return moved;
  }

  void FinishWizard() {
    s.first_run_done = true, s.page = 0, wizard_step_ = 0;
    if (save_settings) SaveSettings(settings_path, s);
  }

  // Session 25 state.
  Hardware hw_;
  GameDataCheck gd_;
  fs::path gd_path_;
  HANDLE game_proc_ = nullptr;
  Command last_cmd_;
  DWORD crash_code_ = 0;
  bool want_crash_popup_ = false, want_wizard_ = false, advanced_open_ = false, force_rtx_ = false;
  bool want_report_popup_ = false;  // automation: --show-report
  bool open_folders_ = true;  // automation: --no-open
  int wizard_step_ = 0, want_mods_tab_ = -1;
  std::string capture_name_ = "my_match";
  fs::path last_report_;
  // Session 58 (Report a problem): the launcher's last messages, the last game exit, the player's note.
  std::vector<std::string> recent_log_;
  std::string last_exit_;
  char report_note_[4096] = {};
  LogoSave logo_, logo_before_;
  bool logo_loaded_ = false, importing_ = false;
  fs::file_time_type logo_stamp_{};
  std::chrono::steady_clock::time_point logo_checked_;
  std::array<ID3D11ShaderResourceView*, kLogoSlots> logo_tex_{};
  struct PngTex {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
  };
  std::map<fs::path, PngTex> png_tex_;
  std::vector<fs::path> logo_picks_;
  int logo_pick_sel_ = 0, backup_sel_ = -1;
  std::vector<LogoJob> logo_jobs_;
  std::map<fs::path, int> logo_forced_;  // automation only
  std::string logo_result_;
  fs::path logo_result_file_;  // Session 27: --wwe2k14_input_result of the logo import
  std::vector<Backup> backups_;


  // ---------------- Trons
  void Select(int tron) {
    for (size_t i = 0; i < chars_.size(); ++i)
      if (chars_[i].tron == tron || (chars_[i].tron < 0 && chars_[i].id == tron)) {
        selected_ = int(i);
        scroll_to_selected_ = true;
        return;
      }
    selected_ = -1, selected_tron_ = tron;  // a tron number with no character in the table
  }

  int SelectedTron() const {
    if (selected_ >= 0 && selected_ < int(chars_.size()))
      return chars_[size_t(selected_)].tron >= 0 ? chars_[size_t(selected_)].tron : chars_[size_t(selected_)].id;
    return selected_tron_;
  }

  void DoAssign(int tron, const fs::path& file, bool stacked_alpha = false) {
    const std::string err = AssignTron(paths_, tron, file, stacked_alpha);
    RefreshOverrides();
    ForgetMedia();
    thumb_.reset();
    Status(err.empty() ? "Tron " + std::to_string(tron) + " now plays " + PathStr(file.filename()) +
                             (s.mods ? "" : "  (switch USE MY VIDEOS on)")
                       : "Could not assign tron " + std::to_string(tron) + ": " + err);
  }

  void DoRevert(int tron) {
    const int n = RevertTron(paths_, tron);
    RefreshOverrides();
    ForgetMedia();
    thumb_.reset();
    Status(n ? "Tron " + std::to_string(tron) + " back to the original" : "Tron " + std::to_string(tron) +
                                                                            " had no override");
  }

  // "original" | "video" | "none" | a video file (copied as NNN.banner.<ext>).
  void DoBanner(int tron, const std::string& choice) {
    const std::string l = Lower(choice);
    std::string err;
    if (l == "original" || l.empty()) err = SetBanner(paths_, tron, BannerMode::kOriginal);
    else if (l == "video") err = SetBanner(paths_, tron, BannerMode::kVideo);
    else if (l == "none") err = SetBanner(paths_, tron, BannerMode::kNone);
    else err = SetBanner(paths_, tron, BannerMode::kFile, fs::path(Widen(choice)));
    banner_tron_ = -1;
    Status(err.empty() ? "Tron " + std::to_string(tron) + " banner ring: " + (l.empty() ? "original" : choice)
                       : "Banner ring not set: " + err);
  }

  void UpdateThumb(const fs::path& f) {
    std::error_code ec;
    if (f.empty()) {
      thumb_.reset();
      return;
    }
    const auto stamp = fs::last_write_time(f, ec);
    if (thumb_ && thumb_->file == f && thumb_->stamp == stamp) return;
    thumb_ = std::make_unique<Thumb>();
    thumb_->file = f, thumb_->stamp = stamp;
    std::vector<uint8_t> rgba;
    thumb_->info = ProbeVideo(f, &rgba);
    if (!rgba.empty() && IsStackedAlpha(f)) UnstackRgba(rgba, thumb_->info.w, thumb_->info.h);
    if (!rgba.empty()) {
      thumb_->srv = MakeTexture(rgba, thumb_->info.w, thumb_->info.h);
      thumb_->w = thumb_->info.w, thumb_->h = thumb_->info.h;
    }
  }

  // ---------------- Tron pictures: still (tile / panel) and hover playback
  // Sources per tron: an assigned MP4 / MOV / MKV (its first frame; hover plays the file itself), else the game-made
  // previews in TronCacheDir() (NNN.png / NNN.mp4: "Build previews"). A 320x320 original is a sheet: the big-screen
  // picture is its top 320x224; the strips below feed the banner screens circling the top of the arena (Jarrod), so
  // only the top part is shown.
  // Disk is looked at once per tron every 2 s (cached), never per frame per tile.
  struct Media {
    fs::path still, video;
    bool sheet_crop = false;  // a game original: crop 320x320 sheets
    std::chrono::steady_clock::time_point checked;
  };
  std::map<int, Media> media_;

  const Media& MediaFor(int tron) {
    const auto now = std::chrono::steady_clock::now();
    auto it = media_.find(tron);
    if (it != media_.end() && now - it->second.checked < std::chrono::seconds(2)) return it->second;
    Media m;
    m.checked = now;
    std::error_code ec;
    const fs::path ov = OverrideFor(paths_, tron);
    if (!ov.empty() && Lower(PathStr(ov.extension())) != ".bik2") {
      m.still = ov, m.video = ov;
    } else if (ov.empty()) {  // the original plays (an assigned .bik2 has no preview here)
      const fs::path png = TronCacheDir() / (std::to_string(tron) + ".png");
      const fs::path mp4 = TronCacheDir() / (std::to_string(tron) + ".mp4");
      const fs::path amp4 = TronCacheDir() / (std::to_string(tron) + ".alpha.mp4");  // a reveal: stacked alpha
      if (fs::exists(png, ec)) m.still = png;
      if (fs::exists(amp4, ec)) m.video = amp4;
      else if (fs::exists(mp4, ec)) m.video = mp4;
      m.sheet_crop = true;
    }
    return media_[tron] = m;
  }
  void ForgetMedia() { media_.clear(); }  // after assign / revert / refresh

  // Draws tron `tron` fitted into [a, b] (letterboxed). Returns false if there is nothing to show.
  bool DrawMedia(ImDrawList* dl, int tron, ImVec2 a, ImVec2 b, bool hovered, float rounding) {
    const Media& m = MediaFor(tron);
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    if (hovered && !m.video.empty() && (hover_video_.empty() || hover_video_ == m.video)) {
      hover_video_ = m.video;
      if (player_.File() != m.video) player_.Open(m.video);
      srv = player_.Tick();
      w = player_.W(), h = player_.H();
    }
    if (!srv && !m.still.empty()) srv = StillImage(tron, m.still, &w, &h);
    if (!srv || !w || !h) return false;
    float v1 = 1.0f, aspect = float(w) / float(h);
    // Rows 0-223 are the big-screen picture (the strips start at row 224: brightness step 223/224 in 343, 100, 139);
    // stop 2 rows short so filtering and H.264's half-size colour don't bleed the strip in.
    if (m.sheet_crop && w == 320 && h == 320) v1 = 222.0f / 320.0f, aspect = 320.0f / 222.0f;
    const float bw = b.x - a.x, bh = b.y - a.y;
    const float iw = aspect >= bw / bh ? bw : bh * aspect, ih = aspect >= bw / bh ? bw / aspect : bh;
    const ImVec2 i0(a.x + (bw - iw) / 2, a.y + (bh - ih) / 2);
    dl->AddImageRounded(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), i0, ImVec2(i0.x + iw, i0.y + ih),
                        ImVec2(0, 0), ImVec2(1, v1), IM_COL32_WHITE, rounding);
    return true;
  }

  // Still pictures: decoded by the ThumbLoader thread, turned into textures here; re-checked on disk every 2 s.
  struct StillTex {
    fs::path file;
    fs::file_time_type stamp;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    std::chrono::steady_clock::time_point checked;
    bool requested = false;
    ~StillTex() { ReleaseLater(srv); }
  };
  std::map<int, std::unique_ptr<StillTex>> stills_;
  ThumbLoader thumbs_;

  // Once per frame: textures for the pictures the loader finished.
  void CollectThumbs() {
    for (auto& d : thumbs_.TakeDone()) {
      auto& t = stills_[d.tron];
      if (!t || t->file != d.file) continue;  // asked for something else meanwhile
      ReleaseLater(t->srv), t->srv = nullptr;
      if (!d.rgba.empty()) t->srv = MakeTexture(d.rgba, d.w, d.h), t->w = d.w, t->h = d.h;
      t->stamp = d.stamp, t->requested = false;
    }
  }

  ID3D11ShaderResourceView* StillImage(int tron, const fs::path& f, int* w, int* h) {
    auto& t = stills_[tron];
    const auto now = std::chrono::steady_clock::now();
    if (!t || t->file != f) {
      t = std::make_unique<StillTex>();
      t->file = f;
    } else if (now - t->checked < std::chrono::seconds(2) || t->requested) {
      *w = t->w, *h = t->h;
      return t->srv;
    }
    t->checked = now;
    std::error_code ec;
    const auto stamp = fs::last_write_time(f, ec);
    if (t->srv && t->stamp == stamp) {
      *w = t->w, *h = t->h;
      return t->srv;
    }
    t->requested = true;
    thumbs_.Request(tron, f, stamp);  // the old picture stays up until the new one is ready
    *w = t->w, *h = t->h;
    return t->srv;
  }

  // "Build previews": one run of the game decodes every original tron with its own Bink (src\tron_export.h).
  void BuildPreviews() {
    std::error_code ec;
    fs::create_directories(TronCacheDir(), ec);
    // Session 53 item 2.2 (a playtester: "--game_data_root was not provided" from Build previews): the game files and
    // save folders as PLAY GAME gives them (the run had none, so a release found no game files).
    const std::string flag = HelperPathFlags() + "--wwe2k14_tron_export=" + Quote(PathStr(TronCacheDir())) +
                             " --wwe2k14_use_launcher_settings=false";  // a helper run keeps the default look
    Command c;
    if (s.harness) {
      c.exe = L"pwsh.exe";
      const std::string a = "-NoProfile -File " + Quote(PathStr(paths_.repo / "tools" / "bench" / "run_list.ps1")) +
                            " -Scripts " + Quote(PathStr(paths_.routes / "manual.txt")) + " -Extra " + Quote(flag);
      c.args = Widen(a), c.display = "pwsh " + a, c.console = true, c.workdir = paths_.repo.wstring();
    } else {
      c.exe = paths_.game_exe.wstring(), c.args = Widen(flag), c.workdir = paths_.launcher_dir.wstring();
      c.display = Quote(PathStr(paths_.game_exe)) + " " + flag;
    }
    const std::string err = RunCommand(c);
    Status(err.empty() ? "Building previews: the game opens, decodes every tron (about 2 minutes) and closes itself"
                       : "Could not start the game: " + err);
  }

  int CachedPreviews() {
    const auto now = std::chrono::steady_clock::now();
    if (now - previews_checked_ > std::chrono::seconds(2)) {
      previews_checked_ = now;
      cached_previews_ = 0;
      std::error_code ec;
      for (const auto& e : fs::directory_iterator(TronCacheDir(), ec))
        cached_previews_ += Lower(PathStr(e.path().extension())) == ".mp4";
    }
    return cached_previews_;
  }

  // The characters shown (search / overrides-only filter), in the current sort order (both views use it).
  std::vector<int> Visible() {
    std::vector<int> out;
    const std::string q = Lower(search_);
    for (size_t i = 0; i < chars_.size(); ++i) {
      const auto& c = chars_[i];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      if (only_overrides_ && !overrides_.count(tron)) continue;
      if (!q.empty() && Lower(c.name).find(q) == std::string::npos && std::to_string(c.id).find(q) != 0 &&
          std::to_string(tron).find(q) != 0)
        continue;
      out.push_back(int(i));
    }
    auto last = [&](int i) {  // no value for the sort column: always at the bottom
      const auto& c = chars_[size_t(i)];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      return (s.trons_sort == 2 && c.tron < 0) || (s.trons_sort == 3 && !overrides_.count(tron) && c.note.empty());
    };
    auto key = [&](int i) -> std::tuple<int, std::string, int> {
      const auto& c = chars_[size_t(i)];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      switch (s.trons_sort) {
        case 1: return {c.id, "", 0};
        case 2: return {c.tron >= 0 ? c.tron : 100000, "", c.id};
        case 3: {
          auto ov = overrides_.find(tron);
          return {ov == overrides_.end() ? 1 : 0, ov == overrides_.end() ? c.note : PathStr(ov->second.extension()),
                  0};
        }
        default: return {0, Lower(c.name), c.id};
      }
    };
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
      if (last(a) != last(b)) return last(b);
      auto ka = key(a), kb = key(b);
      if (ka == kb) return Lower(chars_[size_t(a)].name) < Lower(chars_[size_t(b)].name);
      return s.trons_sort_desc ? kb < ka : ka < kb;
    });
    return out;
  }

  // View buttons (list / grid), drawn at the right end of the current line.
  // List / Grid, right-aligned on the current line (Session 31: for the titles too).
  void ViewButtons(int& view) {
    const float w = 64 * ImGui::GetStyle().FontScaleDpi;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 2 * w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushID(&view);
    for (int v = 0; v < 2; ++v) {
      if (v) ImGui::SameLine();
      const bool on = view == v;
      if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
      if (ImGui::Button(v ? "Grid" : "List", ImVec2(w, 0))) view = v;
      if (on) ImGui::PopStyleColor();
    }
    ImGui::PopID();
  }

  void TronsList(const std::vector<int>& rows) {
    if (!ImGui::BeginTable("chars", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable))
      return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 3.0f);
    ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableSetupColumn("Tron", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
      const ImGuiSortDirection want = s.trons_sort_desc ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending;
      if (sp->SpecsDirty && sp->SpecsCount > 0 && table_sort_ready_) {  // a header click
        s.trons_sort = sp->Specs[0].ColumnIndex;
        s.trons_sort_desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
      } else if (sp->SpecsCount == 0 || sp->Specs[0].ColumnIndex != s.trons_sort ||
                 sp->Specs[0].SortDirection != want) {  // changed elsewhere (saved, grid, automation)
        ImGui::TableSetColumnSortDirection(s.trons_sort, want, false);
      }
      sp->SpecsDirty = false;
      table_sort_ready_ = true;
    }
    for (int i : rows) {
      const auto& c = chars_[size_t(i)];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      const auto ov = overrides_.find(tron);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const bool sel = i == selected_;
      if (ImGui::Selectable((c.name + "##" + std::to_string(i)).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
        selected_ = i;
      if (sel && scroll_to_selected_) ImGui::SetScrollHereY(), scroll_to_selected_ = false;
      ImGui::TableNextColumn();
      ImGui::Text("%d", c.id);
      ImGui::TableNextColumn();
      if (c.tron >= 0) ImGui::Text("%d", c.tron);
      else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      if (ov != overrides_.end())
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", PathStr(ov->second.extension()).c_str() + 1);
      else if (!c.note.empty()) ImGui::TextDisabled("%s", c.note.c_str());
    }
    ImGui::EndTable();
  }

  // Grid: one tile per character (thumbnail, name, tron number); at least 2 columns (Jarrod, Session 31: also at the
  // default window size), more as the window grows.
  void TronsGrid(const std::vector<int>& rows) {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 10 * dpi;
    const int cols = std::max(2, int((avail + gap) / (200 * dpi + gap)));
    const float tile = (avail - gap * float(cols - 1)) / float(cols);
    const float text_h = ImGui::GetTextLineHeightWithSpacing() * 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    hovered_tron_ = -1;
    for (size_t n = 0; n < rows.size(); ++n) {
      const int i = rows[n];
      const auto& c = chars_[size_t(i)];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      if (n % size_t(cols)) ImGui::SameLine(0, gap);
      ImGui::PushID(i);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const bool clicked = ImGui::InvisibleButton("tile", ImVec2(tile, tile + text_h));
      const bool hov = ImGui::IsItemHovered();
      if (clicked) selected_ = i;
      if (i == selected_ && scroll_to_selected_) ImGui::SetScrollHereY(), scroll_to_selected_ = false;
      const ImVec2 a = p, b = ImVec2(p.x + tile, p.y + tile);
      dl->AddRectFilled(a, b, IM_COL32(24, 26, 32, 255), 6 * dpi);
      if (c.tron >= 0 || overrides_.count(tron)) {
        if (hov) hovered_tron_ = tron;
        if (!DrawMedia(dl, tron, a, b, hov || force_hover_ == tron, 0.0f)) {
          const std::string t = std::to_string(tron);
          ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.2f);
          const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
          dl->AddText(ImVec2(a.x + (tile - ts.x) / 2, a.y + (tile - ts.y) / 2), IM_COL32(90, 95, 110, 255), t.c_str());
          ImGui::PopFont();
        }
      } else {
        const char* t = "no tron";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(a.x + (tile - ts.x) / 2, a.y + (tile - ts.y) / 2), IM_COL32(90, 95, 110, 255), t);
      }
      const ImU32 border = i == selected_ ? IM_COL32(230, 70, 60, 255) : hov ? IM_COL32(140, 145, 160, 255)
                                                                              : IM_COL32(60, 64, 76, 255);
      dl->AddRect(a, b, border, 6 * dpi, 0, i == selected_ ? 3 * dpi : 1.5f * dpi);
      if (overrides_.count(tron)) {
        const char* badge = "custom";
        const ImVec2 bs = ImGui::CalcTextSize(badge);
        const ImVec2 b0(b.x - bs.x - 14 * dpi, a.y + 6 * dpi);
        dl->AddRectFilled(b0, ImVec2(b.x - 6 * dpi, b0.y + bs.y + 4 * dpi), IM_COL32(40, 120, 60, 230), 4 * dpi);
        dl->AddText(ImVec2(b0.x + 4 * dpi, b0.y + 2 * dpi), IM_COL32_WHITE, badge);
      }
      const std::string sub = c.tron >= 0 ? "tron " + std::to_string(c.tron) : std::string("no tron");
      ImGui::PushClipRect(ImVec2(a.x, b.y), ImVec2(b.x, b.y + text_h), true);
      dl->AddText(ImVec2(a.x + 2, b.y + 3 * dpi), ImGui::GetColorU32(ImGuiCol_Text), c.name.c_str());
      dl->AddText(ImVec2(a.x + 2, b.y + 3 * dpi + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                  sub.c_str());
      ImGui::PopClipRect();
      ImGui::PopID();
    }
  }

  void TronsTab() {
    const float right_w = 430 * ImGui::GetStyle().FontScaleDpi;
    const ImGuiStyle& st = ImGui::GetStyle();
    float extras = ImGui::CalcTextSize("Overrides only").x + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
                   st.ItemSpacing.x * 2;
    if (s.trons_view == 1) extras += 130 * st.FontScaleDpi + ImGui::GetFrameHeight() + st.ItemSpacing.x * 2;
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - right_w - 8 - extras));
    ImGui::InputTextWithHint("##search", "Search name, id or tron number", &search_);
    ImGui::SameLine();
    ImGui::Checkbox("Overrides only", &only_overrides_);
    if (s.trons_view == 1) {  // the grid has no column headers: sort here
      ImGui::SameLine();
      ImGui::SetNextItemWidth(130 * ImGui::GetStyle().FontScaleDpi);
      const char* keys[] = {"Name", "Id", "Tron", "Override"};
      ImGui::Combo("##sort", &s.trons_sort, keys, 4);
      ImGui::SameLine();
      if (ImGui::ArrowButton("##dir", s.trons_sort_desc ? ImGuiDir_Down : ImGuiDir_Up)) s.trons_sort_desc = !s.trons_sort_desc;
    }
    ViewButtons(s.trons_view);

    // Left: the characters (list or grid).
    const std::vector<int> rows = Visible();
    ImGui::BeginChild("list", ImVec2(-right_w - 8, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    if (s.trons_view == 1) TronsGrid(rows);
    else TronsList(rows);
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the selected character.
    ImGui::BeginChild("detail", ImVec2(right_w, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    const int tron = SelectedTron();
    if (tron < 0) {
      ImGui::TextWrapped("Pick a character on the left.");
      ImGui::Spacing();
      ImGui::TextWrapped("Each wrestler's entrance plays one titantron file, numbered like the wrestler "
                         "(Lesnar 343, Bryan 324). Some share one (The Shield: 403) or use an era version "
                         "(Stone Cold: 658). Assigning a video copies it into the mods folder; the game's own files "
                         "are never changed. Switch USE MY VIDEOS on (above).");
    } else {
      const Character* c = selected_ >= 0 ? &chars_[size_t(selected_)] : nullptr;
      Heading(c ? Upper(c->name).c_str() : "TRON", 1.25f);
      if (c) ImGui::Text("Character id %d, kind %s", c->id, c->kind.c_str());
      ImGui::Text("Titantron %d%s%s", tron, c && !c->note.empty() ? " - " : "", c ? c->note.c_str() : "");
      if (OriginalTronExists(paths_, tron)) {
        if (orig_tron_ != tron) orig_info_ = OriginalTronInfo(paths_, tron), orig_tron_ = tron;
        ImGui::TextDisabled("Original: %s, %dx%d, %.2f fps, %.1f s", orig_info_.format.c_str(), orig_info_.w,
                            orig_info_.h, orig_info_.fps, orig_info_.seconds);
      } else {
        ImGui::TextDisabled("Original: no movies\\titantron\\%d.bik2 in the game files", tron);
      }
      ImGui::Separator();
      const fs::path ov = OverrideFor(paths_, tron);
      UpdateThumb(ov);
      if (ov.empty()) {
        ImGui::TextUnformatted("Plays: the original tron");
        const float w = ImGui::GetContentRegionAvail().x, hgt = w * 224.0f / 320.0f;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, hgt));
        const bool hov = ImGui::IsItemHovered() || force_hover_ == tron;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + hgt), IM_COL32(24, 26, 32, 255), 6);
        if (!DrawMedia(dl, tron, a, ImVec2(a.x + w, a.y + hgt), hov, 0.0f)) {
          const char* t = "No preview yet: Build previews (below)";
          const ImVec2 ts = ImGui::CalcTextSize(t);
          dl->AddText(ImVec2(a.x + (w - ts.x) / 2, a.y + (hgt - ts.y) / 2), IM_COL32(110, 115, 130, 255), t);
        } else if (!hov) {
          ImGui::TextDisabled("Hover the picture to play it");
        }
      } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Plays: %s", PathStr(ov.filename()).c_str());
        if (thumb_) {
          const auto& vi = thumb_->info;
          if (vi.ok) ImGui::TextDisabled("%s, %dx%d, %.2f fps, %.1f s", vi.format.c_str(), vi.w, vi.h, vi.fps, vi.seconds);
          else ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Problem: %s", vi.error.c_str());
          if (thumb_->srv) {
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(thumb_->srv)),
                         ImVec2(w, w * float(thumb_->h) / float(std::max(1, thumb_->w))));
          } else if (vi.ok) {
            ImGui::TextDisabled("(Bink 2: no preview here; it plays in the game)");
          }
        }
        if (Lower(PathStr(ov.extension())) != ".bik2" && orig_info_.w == 320 && orig_info_.h == 320) BannerPanel(tron);
      }
      ImGui::Spacing();
      if (ImGui::Button("Assign a video...", ImVec2(-1, 0))) {
        if (auto f = PickFile(hwnd_)) DoAssign(tron, *f);
      }
      ImGui::BeginDisabled(ov.empty());
      if (ImGui::Button("Revert to the original", ImVec2(-1, 0))) DoRevert(tron);
      ImGui::EndDisabled();
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("MP4 / MOV / MKV: any size (shown up to 1024 wide), looped, no sound. "
                         ".bik2: played by the game's own Bink decoder.");
      ImGui::PopStyleColor();
      // Session 33: this wrestler's entrance music (the game's theme is muted while yours plays).
      if (c) {
        ImGui::Separator();
        const fs::path mu = MusicFor(paths_, c->id);
        if (mu.empty()) ImGui::TextUnformatted("Entrance music: the original");
        else ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Entrance music: %s", PathStr(mu.filename()).c_str());
        if (ImGui::Button("Assign music...", ImVec2(-1, 0))) {
          if (auto f = PickAudio(hwnd_)) {
            const std::string err = AssignMusic(paths_, c->id, *f);
            Status(err.empty() ? "Entrance music assigned: " + PathStr(f->filename()) : "Music not assigned: " + err);
          }
        }
        ImGui::BeginDisabled(mu.empty());
        if (ImGui::Button("Original music", ImVec2(-1, 0))) RevertMusic(paths_, c->id), Status("Original entrance music");
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("MP3 / M4A / WAV / FLAC / WMA / AAC. Plays in normal play (not in replays); copied into "
                           "the mods folder as music\\%d. Needs USE MY VIDEOS on.", c->id);
        ImGui::PopStyleColor();
      }
    }
    ImGui::EndChild();

    // Bottom row (Session 31: the folder's path in the button's tooltip, so the row fits the window).
    ImGui::Text("%zu override(s)", overrides_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open mods folder")) {
      std::error_code ec;
      fs::create_directories(paths_.mods / "titantron", ec);
      ShellExecuteW(hwnd_, L"explore", (paths_.mods / "titantron").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PathStr(paths_.mods / "titantron").c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(overrides_.empty());
    if (ImGui::SmallButton("Revert all...")) ImGui::OpenPopup("revert_all");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshOverrides(), thumb_.reset(), stills_.clear(), ForgetMedia();
    ImGui::SameLine();
    {
      const int n = CachedPreviews();
      if (ImGui::SmallButton(n ? "Rebuild previews" : "Build previews")) BuildPreviews();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs the game once: its own Bink decoder makes a thumbnail and a faithful MP4 copy of every\n"
                          "original tron (every frame, full length, original size; about 2-3 minutes, ~0.5 GB; the\n"
                          "game closes itself). Saved in\n%s",
                          PathStr(TronCacheDir()).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("%d preview(s)", n);
    }
    if (ImGui::BeginPopupModal("revert_all", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Remove all %zu tron override(s) from the mods folder?", overrides_.size());
      if (ImGui::Button("Remove all")) {
        Status("Reverted " + std::to_string(RevertAll(paths_)) + " file(s)");
        RefreshOverrides(), thumb_.reset(), ForgetMedia();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }

  // ---------------- Banner ring (Session 18): the strips under a 320x320 tron's big screen
  void BannerPanel(int tron) {
    if (banner_tron_ != tron || std::chrono::steady_clock::now() - banner_checked_ > std::chrono::seconds(2))
      banner_ = GetBanner(paths_, tron), banner_tron_ = tron, banner_checked_ = std::chrono::steady_clock::now();
    ImGui::Spacing();
    ImGui::SeparatorText("Banner ring");
    const char* names[] = {"Original (the wrestler's own)", "From my video (a band of it)", "None (black)",
                           "Separate video..."};
    int cur = banner_.mode == BannerMode::kOriginal ? 0 : banner_.mode == BannerMode::kVideo ? 1
              : banner_.mode == BannerMode::kNone ? 2 : 3;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##banner", cur == 3 ? ("Separate: " + PathStr(banner_.file.filename())).c_str() : names[cur])) {
      for (int i = 0; i < 4; ++i)
        if (ImGui::Selectable(names[i], i == cur)) {
          if (i == 3) {
            if (auto f = PickFile(hwnd_)) DoBanner(tron, PathStr(*f));
          } else {
            DoBanner(tron, i == 0 ? "original" : i == 1 ? "video" : "none");
          }
        }
      ImGui::EndCombo();
    }
    // The sheet as the game gets it: the big screen (top 224 rows) and the four 24-row strips.
    const float w = ImGui::GetContentRegionAvail().x, big_h = w * kSheetBigRows / 320.0f;
    const float strip_h = w * kSheetStripRows / 320.0f;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, big_h + strip_h * kSheetStrips));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + big_h + strip_h * kSheetStrips), IM_COL32(8, 8, 10, 255));
    if (thumb_ && thumb_->srv)
      dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(thumb_->srv)), a, ImVec2(a.x + w, a.y + big_h));
    const float y0 = a.y + big_h;
    if (banner_.mode == BannerMode::kOriginal) {
      int sw = 0, sh = 0;
      const fs::path png = TronCacheDir() / (std::to_string(tron) + ".png");
      std::error_code ec;
      if (auto* srv = fs::exists(png, ec) ? StillImage(tron + 200000, png, &sw, &sh) : nullptr)
        dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), ImVec2(a.x, y0),
                     ImVec2(a.x + w, y0 + strip_h * kSheetStrips), ImVec2(0, kSheetBigRows / 320.0f), ImVec2(1, 1));
      else
        dl->AddText(ImVec2(a.x + 6, y0 + 4), IM_COL32(110, 115, 130, 255), "(original strips: Build previews to see)");
    } else if (banner_.mode != BannerMode::kNone) {
      ID3D11ShaderResourceView* srv = nullptr;
      float v0 = 0, v1 = 1;
      bool split = false;  // a wide banner video runs around the ring: one quarter per strip (as the game does)
      if (banner_.mode == BannerMode::kVideo) {
        srv = thumb_ ? thumb_->srv : nullptr;
        const float band = float(kSheetStripRows) / float(kSheetBigRows);
        v0 = 0.5f - band / 2, v1 = 0.5f + band / 2;
      } else {
        int sw = 0, sh = 0;
        srv = StillImage(tron + 100000, banner_.file, &sw, &sh);
        split = sh > 0 && sw * kSheetStripRows > sh * 2 * 320;
      }
      for (int i = 0; srv && i < kSheetStrips; ++i) {
        const float u0 = split ? i / float(kSheetStrips) : 0.0f, u1 = split ? (i + 1) / float(kSheetStrips) : 1.0f;
        dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), ImVec2(a.x, y0 + strip_h * i),
                     ImVec2(a.x + w, y0 + strip_h * (i + 1)), ImVec2(u0, v0), ImVec2(u1, v1));
      }
    }
    for (int i = 0; i <= kSheetStrips; ++i)
      dl->AddLine(ImVec2(a.x, y0 + strip_h * i), ImVec2(a.x + w, y0 + strip_h * i), IM_COL32(0, 0, 0, 200), 1.0f);
    ImGui::TextDisabled("Top: the big screen. Below: the 4 strips; the banner ring around the arena shows them end to "
                        "end (1, 2, 3, 4, 1, ...). A separate banner video wider than 27:1 runs around the ring.");
  }

  // ---------------- Titles (Session 18): the title reveal videos (Bink 1 with alpha, 880x720)
  struct Title {
    int id = 0;
    std::string name;
  };
  std::vector<Title> titles_;
  bool titles_loaded_ = false;
  int title_sel_ = -1;
  bool title_alpha_ = true;  // assign: the video is stacked alpha
  BannerChoice banner_;
  int banner_tron_ = -1;
  std::chrono::steady_clock::time_point banner_checked_;

  void LoadTitles() {
    titles_loaded_ = true;
    titles_.clear();
    static const std::map<int, const char*> known = {
        {500, "WWE Championship"}, {950, "WWE Championship (older design)"}, {970, "United States Championship"}};
    // Session 26: through GameFiles (a folder or a disc image).
    for (const std::string& name : GameFilesAt(paths_.game_data).List("movies\\titantron")) {
      const fs::path file(name);
      const std::string stem = PathStr(file.stem());
      if (Lower(PathStr(file.extension())) != ".bik2" || stem.empty() ||
          !std::all_of(stem.begin(), stem.end(), ::isdigit))
        continue;
      const int id = std::stoi(stem);
      const VideoInfo vi = OriginalTronInfo(paths_, id);
      if (!vi.ok || !vi.alpha) continue;  // only the files that are transparent in the game itself
      auto k = known.find(id);
      titles_.push_back({id, k != known.end() ? k->second : "Title reveal " + stem});
    }
    std::sort(titles_.begin(), titles_.end(), [](const Title& x, const Title& y) { return x.id < y.id; });
  }

  // A checkerboard behind transparent pictures.
  static void Checker(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell) {
    dl->AddRectFilled(a, b, IM_COL32(70, 72, 78, 255));
    for (float y = a.y; y < b.y; y += cell)
      for (float x = a.x + (int((y - a.y) / cell) % 2 ? cell : 0); x < b.x; x += 2 * cell)
        dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, b.x), std::min(y + cell, b.y)),
                          IM_COL32(110, 112, 120, 255));
  }

  // Session 31 (Jarrod): the titles as a grid too: tiles in the videos' shape (880 x 720) over a checkerboard (their
  // transparency), name and file below, at least 2 columns; hover plays the preview, a click selects.
  void TitlesGrid() {
    const float dpi = Dpi(), avail = ImGui::GetContentRegionAvail().x, gap = 10 * dpi;
    const int cols = std::max(2, int((avail + gap) / (200 * dpi + gap)));
    const float tw = (avail - gap * float(cols - 1)) / float(cols), th = tw * 720.0f / 880.0f;
    const float text_h = ImGui::GetTextLineHeightWithSpacing() * 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t n = 0; n < titles_.size(); ++n) {
      const Title& t = titles_[n];
      if (n % size_t(cols)) ImGui::SameLine(0, gap);
      ImGui::PushID(t.id);
      const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + tw, a.y + th);
      if (ImGui::InvisibleButton("tile", ImVec2(tw, th + text_h))) title_sel_ = t.id;
      const bool hov = ImGui::IsItemHovered() || force_hover_ == t.id;
      Checker(dl, a, b, 10 * dpi);
      if (!DrawMedia(dl, t.id, a, b, hov, 0.0f)) {
        const std::string s_id = std::to_string(t.id);
        const ImVec2 ts = ImGui::CalcTextSize(s_id.c_str());
        dl->AddText(ImVec2(a.x + (tw - ts.x) / 2, a.y + (th - ts.y) / 2), IM_COL32(230, 230, 235, 255), s_id.c_str());
      }
      const bool sel = t.id == title_sel_;
      dl->AddRect(a, b, sel ? IM_COL32(230, 70, 60, 255) : hov ? IM_COL32(140, 145, 160, 255) : IM_COL32(60, 64, 76, 255),
                  0, 0, sel ? 3 * dpi : 1.5f * dpi);
      if (overrides_.count(t.id)) {
        const char* badge = "custom";
        const ImVec2 bs = ImGui::CalcTextSize(badge), b0(b.x - bs.x - 14 * dpi, a.y + 6 * dpi);
        dl->AddRectFilled(b0, ImVec2(b.x - 6 * dpi, b0.y + bs.y + 4 * dpi), IM_COL32(40, 120, 60, 230), 4 * dpi);
        dl->AddText(ImVec2(b0.x + 4 * dpi, b0.y + 2 * dpi), IM_COL32_WHITE, badge);
      }
      const std::string sub = "file " + std::to_string(t.id);
      ImGui::PushClipRect(ImVec2(a.x, b.y), ImVec2(b.x, b.y + text_h), true);
      dl->AddText(ImVec2(a.x + 2, b.y + 3 * dpi), ImGui::GetColorU32(ImGuiCol_Text), t.name.c_str());
      dl->AddText(ImVec2(a.x + 2, b.y + 3 * dpi + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                  sub.c_str());
      ImGui::PopClipRect();
      ImGui::PopID();
    }
  }

  void TitlesTab() {
    if (!titles_loaded_) LoadTitles();
    const float dpi = ImGui::GetStyle().FontScaleDpi, right_w = 430 * dpi;
    ImGui::TextDisabled("%zu title reveal(s)", titles_.size());
    ViewButtons(s.titles_view);
    ImGui::TextWrapped("Title reveal videos: the belt animations shown with a champion's entrance. They are "
                       "transparent in the game. A replacement keeps its transparency when it is a stacked-alpha "
                       "video (colour on top, alpha as grey below; make one with tools\\trons\\make_stacked_alpha.py).");
    ImGui::BeginChild("tlist", ImVec2(-right_w - 8, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    if (s.titles_view == 1) {
      TitlesGrid();
    } else if (ImGui::BeginTable("titles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                  ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 0.7f);
      ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthStretch, 1.4f);
      ImGui::TableHeadersRow();
      for (const auto& t : titles_) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable((t.name + "##t" + std::to_string(t.id)).c_str(), t.id == title_sel_,
                              ImGuiSelectableFlags_SpanAllColumns))
          title_sel_ = t.id;
        ImGui::TableNextColumn();
        ImGui::Text("%d", t.id);
        ImGui::TableNextColumn();
        auto ov = overrides_.find(t.id);
        if (ov != overrides_.end())
          ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", TronExt(ov->second).c_str() + 1);
      }
      ImGui::EndTable();
    }
    if (titles_.empty()) ImGui::TextDisabled("No title reveals found in %s", PathStr(paths_.game_data).c_str());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("tdetail", ImVec2(right_w, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    const Title* t = nullptr;
    for (const auto& x : titles_)
      if (x.id == title_sel_) t = &x;
    if (!t) {
      ImGui::TextWrapped("Pick a title on the left. 500 = WWE Championship, 970 = United States Championship; the "
                         "950s are probably the retro titles of 30 Years of WrestleMania.");
    } else {
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
      ImGui::TextUnformatted(t->name.c_str());
      ImGui::PopFont();
      const VideoInfo vi = OriginalTronInfo(paths_, t->id);
      ImGui::TextDisabled("File %d.bik2: %s, %dx%d, %.2f fps, %.1f s", t->id, vi.format.c_str(), vi.w, vi.h, vi.fps,
                          vi.seconds);
      const fs::path ov = OverrideFor(paths_, t->id);
      if (ov.empty()) ImGui::TextUnformatted("Plays: the original");
      else ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Plays: %s%s", PathStr(ov.filename()).c_str(),
                              IsStackedAlpha(ov) ? " (transparent)" : " (no transparency: solid)");
      const float w = ImGui::GetContentRegionAvail().x, hgt = w * 720.0f / 880.0f;
      const ImVec2 a = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(w, hgt));
      const bool hov = ImGui::IsItemHovered() || force_hover_ == t->id;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      Checker(dl, a, ImVec2(a.x + w, a.y + hgt), 12 * dpi);
      if (!DrawMedia(dl, t->id, a, ImVec2(a.x + w, a.y + hgt), hov, 0.0f)) {
        const char* m = "No preview yet: Build previews (Trons)";
        const ImVec2 ts = ImGui::CalcTextSize(m);
        dl->AddText(ImVec2(a.x + (w - ts.x) / 2, a.y + (hgt - ts.y) / 2), IM_COL32(230, 230, 235, 255), m);
      } else if (!hov) {
        ImGui::TextDisabled("Hover the picture to play it (checkerboard = transparent)");
      }
      ImGui::Spacing();
      ImGui::Checkbox("My video is stacked alpha (keeps transparency)", &title_alpha_);
      if (ImGui::Button("Assign a video...", ImVec2(-1, 0))) {
        if (auto f = PickFile(hwnd_)) DoAssign(t->id, *f, title_alpha_);
      }
      ImGui::BeginDisabled(ov.empty());
      if (ImGui::Button("Revert to the original", ImVec2(-1, 0))) DoRevert(t->id);
      ImGui::EndDisabled();
    }
    ImGui::EndChild();
  }

  // ---------------- Play
  bool Combo(const char* label, std::string& value, const std::vector<std::string>& items, const char* none) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.empty() ? none : value.c_str())) {
      if (none && ImGui::Selectable(none, value.empty())) value.clear(), changed = true;
      for (const auto& it : items)
        if (ImGui::Selectable(it.c_str(), it == value)) value = it, changed = true;
      ImGui::EndCombo();
    }
    return changed;
  }

  std::string CharName(int id) const {
    for (const auto& c : chars_)
      if (c.id == id) return c.name;
    for (const auto& c : dlc_chars_)  // Session 32
      if (c.id == id) return c.name + (DlcWrestlerOn(c) ? "" : " (DLC pack off)");
    return "#" + std::to_string(id);
  }

  // Back to a clean Play tab: every match / preset / option setting to its default. Paths, the window size and the
  // Trons tab's view stay.
  void ResetPlay() {
    Settings d;
    d.game_exe = s.game_exe, d.mods_dir = s.mods_dir, d.window_w = s.window_w, d.window_h = s.window_h;
    d.trons_view = s.trons_view, d.trons_sort = s.trons_sort, d.trons_sort_desc = s.trons_sort_desc;
    s = d;
    rec_mode_ = 0;
    seeds_scenario_ = "\x01";
    if (save_settings) SaveSettings(settings_path, s);
    Status("Play settings reset to the defaults");
  }

  // Session 25: the Sessions 17-24 Play tab, now Play Game -> Advanced (the match itself is Quick Match's).
  void AdvancedPlay() {
    const float label_w = 170 * ImGui::GetStyle().FontScaleDpi;
    if (ImGui::Button("Reset these settings")) ImGui::OpenPopup("reset_play");
    if (ImGui::BeginPopupModal("reset_play", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("Reset the match, preset and option settings to their defaults?");
      ImGui::TextDisabled("Paths, your game files, trons, logos and Settings stay.");
      if (ImGui::Button("Reset")) ResetPlay(), ImGui::CloseCurrentPopup();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::SeparatorText("Preset");
    for (int i = 0; i < kPresetCount; ++i) {
      if (i) ImGui::SameLine();
      ImGui::RadioButton(kPresetNames[i], &s.preset, i);
    }
    switch (s.preset) {
      case kPlay:
        ImGui::TextDisabled("The game with your settings (and the Quick Match above, if one is picked).");
        break;
      case kReplay:
        ImGui::RadioButton("Record a new match", &rec_mode_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Play a recording", &rec_mode_, 1);
        s.replay_record = rec_mode_ == 0;
        ImGui::SetNextItemWidth(label_w * 2);
        if (s.replay_record) ImGui::InputText("Recording name", &s.replay_name);
        else Combo("Recording", s.replay_play, replays_, nullptr);
        ImGui::TextDisabled("Replay mode: the run is exactly repeatable (out\\replays\\NAME.rec).");
        break;
      case kReshoot: {
        ImGui::SetNextItemWidth(label_w * 2);
        Combo("Recording", s.replay_play, replays_, nullptr);
        ImGui::SetNextItemWidth(label_w * 2);
        ImGui::InputTextWithHint("Shots", "a.txt,b.txt (CineCam paths)", &s.reshoot_shots);
        ImGui::SameLine();
        if (ImGui::SmallButton("Add shot...")) ImGui::OpenPopup("pick_shot");
        if (ImGui::BeginPopup("pick_shot")) {
          for (const auto& sh : shots_)
            if (ImGui::Selectable(sh.c_str())) s.reshoot_shots += (s.reshoot_shots.empty() ? "" : ",") + sh;
          if (shots_.empty()) ImGui::TextDisabled("No paths in %s", PathStr(paths_.shots).c_str());
          ImGui::EndPopup();
        }
        ImGui::SetNextItemWidth(label_w);
        ImGui::InputText("Anchor", &s.reshoot_anchor);
        ImGui::TextDisabled("One pass per shot, fast-forwarded -> out\\reshoots\\NAME\\shotNN.mp4");
        break;
      }
      case kSeed:
        SeedPanel(label_w);
        break;
      case kDirector:
        ImGui::SetNextItemWidth(label_w * 2);
        Combo("Director script", s.director, directors_, nullptr);
        ImGui::SameLine();
        if (ImGui::SmallButton("Open folder")) {
          std::error_code ec;
          fs::create_directories(paths_.director, ec);
          ShellExecuteW(hwnd_, L"explore", paths_.director.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::TextDisabled("out\\director\\NAME.txt: scenario, seed and timed events; runs in replay mode.");
        ImGui::Checkbox("Use this seed instead of the script's", &s.director_seed);
        SeedPanel(label_w);
        break;
    }
    ImGui::SeparatorText("Options");
    ImGui::Checkbox("Mods (custom trons and titles)", &s.mods);
    ImGui::SameLine();
    ImGui::Checkbox("CineCam (F8)", &s.cinecam);
    if (paths_.repo_found) {
      ImGui::Checkbox("Test harness (frozen save, run lock)", &s.harness);
      if (s.harness) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(label_w * 2);
        Combo("Route", s.harness_route, routes_, nullptr);
      }
    }
    ImGui::SeparatorText("Command");
    const Command cmd = BuildCommand(s, paths_);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild("cmd", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 2.8f), ImGuiChildFlags_None);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(cmd.display.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(cmd.display.c_str()), Status("Command copied");
    ImGui::SameLine();
    if (!cmd.error.empty()) ImGui::TextColored(ImVec4(1, 0.55f, 0.4f, 1), "%s", cmd.error.c_str()), ImGui::SameLine();
    ImGui::BeginDisabled(!cmd.error.empty() || game_proc_ != nullptr);
    if (ImGui::Button("Launch with this preset")) DoLaunch(false);
    ImGui::EndDisabled();
  }

  void SeedPanel(float label_w) {
    ImGui::SetNextItemWidth(label_w);
    ImGui::InputInt("Seed", &s.seed);
    ImGui::SameLine();
    ImGui::TextDisabled("plays the scenario in replay mode with this seed");
    const std::string key = s.scenario + (s.preset == kDirector && !s.director.empty() ? "+" + s.director : "");
    if (seeds_scenario_ != key) seeds_ = LoadSeeds(paths_, key), seeds_scenario_ = key;
    ImGui::SetNextItemWidth(label_w * 2);
    ImGui::InputTextWithHint("Filter", "winner=0 finish=1 max_clock=300 min_near_falls=2", &s.seed_filter);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) seeds_scenario_.clear();
    if (ImGui::BeginTable("seeds", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 7))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      for (const char* h : {"seed", "finish", "winner", "clock s", "pins", "near falls", "run"}) ImGui::TableSetupColumn(h);
      ImGui::TableHeadersRow();
      int shown = 0;
      for (const auto& r : seeds_) {
        if (!SeedMatches(r, s.seed_filter)) continue;
        ++shown;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const int seed = std::atoi(r.get("seed").c_str());
        if (ImGui::Selectable((r.get("seed") + "##s").c_str(), seed == s.seed, ImGuiSelectableFlags_SpanAllColumns))
          s.seed = seed;
        for (const char* k : {"finish", "winner", "clock_s", "pins", "near_falls", "run"}) {
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(r.get(k).c_str());
        }
      }
      ImGui::EndTable();
      if (seeds_.empty()) ImGui::TextDisabled("No sweep for this scenario yet (out\\seeds\\%s.csv).", s.scenario.c_str());
      else ImGui::TextDisabled("%d of %zu seeds match.", shown, seeds_.size());
    }
    ImGui::SetNextItemWidth(label_w * 0.7f);
    ImGui::InputInt("from", &s.sweep_from, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(label_w * 0.7f);
    ImGui::InputInt("to", &s.sweep_to, 0);
    ImGui::SameLine();
    if (ImGui::Button("Run a sweep")) DoLaunch(true);
    ImGui::SameLine();
    ImGui::TextDisabled("one fast-forwarded CPU match per seed (tools\\seeds\\sweep.py)");
  }

  void DoLaunch(bool sweep) {
    const Command cmd = BuildCommand(s, paths_, sweep);
    StartGame(cmd, "Started: " + cmd.display);
  }

  HWND hwnd_;
  Paths paths_;
  std::vector<Character> chars_;
  std::vector<Arena> arenas_;
  std::vector<Scenario> scenarios_;
  std::vector<std::string> replays_, directors_, shots_, routes_;
  std::map<int, fs::path> overrides_;
  std::vector<SeedRow> seeds_;
  std::string seeds_scenario_ = "\x01";
  std::string search_, pick_search_, status_;
  std::mt19937 rng_{std::random_device{}()};  // Session 27: Quick Match "Random"
  std::chrono::steady_clock::time_point status_time_;
  bool only_overrides_ = false, scroll_to_selected_ = false;
  int selected_ = -1, selected_tron_ = -1, want_tab_ = -1, orig_tron_ = -1;
  int rec_mode_ = 0;
  int hovered_tron_ = -1;  // grid tile under the mouse (its preview plays)
  int force_hover_ = -1;   // automation: --hover TRON (screenshots of playback)
  Player player_;
  fs::path hover_video_;   // the video asked for this frame (the player stops when nothing is hovered)
  std::chrono::steady_clock::time_point previews_checked_;
  int cached_previews_ = 0;
  bool table_sort_ready_ = false;  // the table's first sort specs are its defaults, not a click
  VideoInfo orig_info_;
  std::unique_ptr<Thumb> thumb_;
  // Session 31: the look (background picture, logo, the game's menu face).
  ID3D11ShaderResourceView* bg_srv_ = nullptr;
  int bg_w_ = 0, bg_h_ = 0;
  std::vector<uint8_t> logo_px_;
  int logo_w_ = 0, logo_h_ = 0;
  bool logo_tried_ = false;
  ID3D11ShaderResourceView* logo_srv_[3] = {};  // slots: Play screen, About, the band
  int logo_srv_w_[3] = {};
  GameFont gfont_;
  std::vector<Gpu> gpus_;  // Settings -> Display: the graphics cards (ListGpus)
  // Session 37: the Vulkan devices (vk_devices.h), listed once; the one the game will use for a GRAPHICS CARD choice.
  std::vector<wwe2k14::vkdev::Device> vk_devs_;
  bool vk_listed_ = false;
  bool fake_igpu_ = false;  // automation: --fake-igpu (the integrated-graphics choices on any PC, for screenshots)
  bool fake_no_interlock_ = false;  // automation (Session 54): --fake-no-interlock (a card without interlock)
  wwe2k14::vkdev::Device ChosenDevice(const std::string& gpu) {
    if (!vk_listed_) vk_devs_ = wwe2k14::vkdev::List(), vk_listed_ = true;
    const int i = wwe2k14::vkdev::Pick(vk_devs_, gpu);
    wwe2k14::vkdev::Device d = i >= 0 ? vk_devs_[size_t(i)] : wwe2k14::vkdev::Device{};
    if (fake_igpu_) d.type = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, d.name = "(test) integrated GPU";
    if (fake_no_interlock_) d.interlock = false, d.name = "(test) card without interlock";
    return d;
  }
  bool nv_other_ = false;  // Settings -> Graphics: the player opened "Use other files..." (Session 31)
  ID3D11ShaderResourceView* gfont_srv_ = nullptr;
  fs::path gfont_root_;
  bool gfont_tried_ = false;
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
    else if (a == "--update-file") app.update_file_ = next(), app.update_force_ = true;  // GitHub's answer from a file
    else if (a == "--update-version") app.update_version_ = next();   // pretend this version
    else if (a == "--show-update") out.push_back({"show_update"});
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
