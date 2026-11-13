// A plain window for the player.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace rdr {
struct NativeWindow {
  void* instance;
  void* window;
};
}  // namespace rdr

namespace {
bool g_open = true;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CLOSE) {
    g_open = false;
    return 0;
  }
  return DefWindowProcA(window, message, wparam, lparam);
}
}  // namespace

bool OpenTestWindow(int width, int height, rdr::NativeWindow& out) {
  HINSTANCE instance = GetModuleHandleA(nullptr);
  WNDCLASSA window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = "rplay";
  RegisterClassA(&window_class);
  RECT rect{0, 0, width, height};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("rplay", "rplay", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
  if (!window) return false;
  out.instance = instance;
  out.window = window;
  return true;
}

bool PumpTestWindow() {
  MSG message;
  while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }
  return g_open;
}
#endif
