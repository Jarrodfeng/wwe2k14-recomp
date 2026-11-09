// Session 98 (phone port, stage A): the Win32 window of the viewer (platform.h). Moved out of viewer_main.cpp (WndProc, the
// right-button mouse look, the window title, the cursor / focus queries); input_win32.h's keys and XInput pad are used from here.
#include <windows.h>
#include <windowsx.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cstdlib>

#include "input_win32.h"
#include "platform.h"
#include "renderer_d3d11.h"
#include "ui_backend_win32_dx11.h"

namespace view {

namespace {

std::wstring Wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return L"";
  std::wstring w((size_t)n - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

class Win32Platform : public Platform {
 public:
  const char* Name() const override { return "win32"; }
  bool CreateMainWindow(const char* title, int w, int h, std::string& err) override {
    ui::EnableDpiAwareness();  // Session 98
    WNDCLASSA wc = {};
    wc.lpfnWndProc = &Win32Platform::WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "wwe2k14_assets_viewer";
    RegisterClassA(&wc);
    RECT rc = {0, 0, w, h};
    const float ds = ui::SystemDpiScale();  // Session 98: per-monitor DPI aware: the window is w x h at 96 dpi, scaled up on high-DPI screens
    rc.right = (LONG)(w * ds), rc.bottom = (LONG)(h * ds);
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    self_ = this;
    hwnd_ = CreateWindowA(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                          rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) { err = "CreateWindow failed"; return false; }
    return true;
  }
  Surface MakeSurface() override { Surface s; s.hwnd = hwnd_; return s; }
  void ClientSize(int& w, int& h) override {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    w = cr.right - cr.left, h = cr.bottom - cr.top;
  }
  void Pump(InputSink& sink) override {
    sink_ = &sink;
    MSG m;
    while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
  }
  bool Minimized() override { return hwnd_ && IsIconic(hwnd_); }
  bool Focused() override { return hwnd_ && GetForegroundWindow() == hwnd_; }
  void SetTitle(const std::string& t) override { if (hwnd_) SetWindowTextW(hwnd_, Wide(t).c_str()); }
  bool CursorInClient(int& x, int& y) override {
    if (!hwnd_) return false;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    x = pt.x, y = pt.y;
    return pt.x >= 0 && pt.y >= 0 && pt.x < rc.right && pt.y < rc.bottom && GetForegroundWindow() == hwnd_;
  }
  // The right-button look (Session 98 freecam): cursor hidden, pinned to the window centre; the saved screen position comes back on release.
  void SetMouseLook(bool on) override {
    if (on == looking_ || !hwnd_) return;
    if (on) {
      looking_ = true;
      GetCursorPos(&look_saved_);
      ::SetCapture(hwnd_);
      ShowCursor(FALSE);
      const POINT c = LookCentre();
      SetCursorPos(c.x, c.y);
    } else {
      looking_ = false;  // (first: ReleaseCapture sends WM_CAPTURECHANGED, which comes back to the viewer)
      ReleaseCapture();
      SetCursorPos(look_saved_.x, look_saved_.y);
      ShowCursor(TRUE);
    }
  }
  void SetCapture(bool on) override {
    if (!hwnd_) return;
    if (on) ::SetCapture(hwnd_);
    else ReleaseCapture();
  }
  bool KeyHeld(unsigned vk) override { return (GetKeyState((int)vk) & 0x8000) != 0; }
  void PollFreeKeys(FreeKeys& k, bool focused) override { view::PollFreeKeys(k, focused); }
  void PollPad(FreeCamCtl& c, bool focused) override { pad_.Poll(c, focused); }
  void Destroy() override {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  std::unique_ptr<ui::Backend> MakeUiBackend(Renderer* r, std::string& err) override {
    auto* d3d = dynamic_cast<RendererD3D11*>(r);
    if (!d3d) { err = "the Win32 window's UI needs the d3d11 renderer"; return nullptr; }
    return ui::MakeWin32Dx11Backend(hwnd_, d3d);
  }

 private:
  POINT LookCentre() const {  // the client centre in screen coordinates
    RECT rc;
    GetClientRect(hwnd_, &rc);
    POINT p = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    ClientToScreen(hwnd_, &p);
    return p;
  }
  // Mouse look: the move since the centre is the delta; the cursor goes back to the centre.
  void LookMove(InputSink& s) {
    const POINT c = LookCentre();
    POINT p;
    GetCursorPos(&p);
    if (p.x == c.x && p.y == c.y) return;
    s.OnLookDelta(float(p.x - c.x), float(p.y - c.y));
    SetCursorPos(c.x, c.y);
  }
  static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    Win32Platform* p = self_;
    InputSink* s = p ? p->sink_ : nullptr;
    if (!s) return DefWindowProcA(hw, msg, wp, lp);
    {  // Session 98: the UI sees every message first; it swallows mouse / keys it wants
      intptr_t uo = 0;
      const NativeMsg nm{hw, msg, (uintptr_t)wp, (intptr_t)lp};
      if (s->UiFirst(&nm, uo)) return (LRESULT)uo;
    }
    switch (msg) {
      case WM_CLOSE: s->OnClose(); return 0;
      case WM_SIZE:
        if (wp != SIZE_MINIMIZED) s->OnResize(LOWORD(lp), HIWORD(lp));
        return 0;
      case WM_LBUTTONDOWN: s->OnMouseDown(0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
      case WM_LBUTTONUP: s->OnMouseUp(0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
      case WM_RBUTTONDOWN: s->OnMouseDown(1, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
      case WM_RBUTTONUP: s->OnMouseUp(1, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
      case WM_CAPTURECHANGED: if ((HWND)lp != hw) s->OnCaptureLost(); return 0;
      case WM_KILLFOCUS: s->OnFocusLost(); return 0;
      case WM_MOUSEMOVE:
        if (p->looking_) { p->LookMove(*s); return 0; }
        s->OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
      case WM_MOUSEWHEEL: s->OnWheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA); return 0;
      case WM_KEYDOWN: s->OnKey((unsigned)wp, (lp & (1 << 30)) != 0); return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
  }
  static Win32Platform* self_;
  HWND hwnd_ = nullptr;
  InputSink* sink_ = nullptr;
  bool looking_ = false;
  POINT look_saved_ = {0, 0};
  Pad pad_;
};
Win32Platform* Win32Platform::self_ = nullptr;

}  // namespace

std::unique_ptr<Platform> MakeWin32Platform() { return std::make_unique<Win32Platform>(); }

// UTF-8 (the callers use fs::u8path): the ANSI environment read lost a non-ASCII user name's folder (review of 8dcd6d7b).
std::string LocalDataDir() {
  std::string out;
  PWSTR w = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &w)) && w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) { out.resize((size_t)n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr); }
  }
  if (w) CoTaskMemFree(w);
  return out.empty() ? std::string(".") : out;
}

}  // namespace view
