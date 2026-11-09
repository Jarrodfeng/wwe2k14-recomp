// Session 98 (phone port, stage A): the viewer's window + input seam. viewer_main.cpp holds no window-system code; a Platform makes
// the window, turns its events into InputSink calls and answers the few questions the viewer asks (focus, cursor, held keys, pad).
//   platform_win32.cpp: Win32 window (the D3D11 renderer; the PC default).
//   platform_sdl.cpp  : SDL3 window (the Vulkan renderer on the PC; the phone port's platform).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "freecam.h"
#include "renderer.h"
#include "ui_backend.h"

namespace view {

// Key codes: the Win32 virtual-key numbers on every platform ('A'..'Z' and '0'..'9' are their ASCII upper-case / digit codes);
// the SDL platform maps its keys to these. (ui_timeline.cpp uses the same numbers.)
namespace key {
constexpr unsigned kTab = 0x09, kEscape = 0x1B, kSpace = 0x20, kPrior = 0x21, kNext = 0x22, kLeft = 0x25, kUp = 0x26, kRight = 0x27,
                   kDown = 0x28, kShift = 0x10, kControl = 0x11, kOem4 = 0xDB /* [ */, kOem6 = 0xDD /* ] */, kComma = 0xBC,
                   kPeriod = 0xBE, kMinus = 0xBD, kPlus = 0xBB;
}

// What the viewer does with the window's events (viewer_main.cpp Viewer).
class InputSink {
 public:
  virtual ~InputSink() {}
  // Every event goes to the UI first: true = the UI used it (Win32: `out` is the window procedure's result). `native` is the
  // platform's event: Win32 = NativeMsg, SDL = const SDL_Event*; see ui::Backend::Message.
  virtual bool UiFirst(const void* native, intptr_t& out) = 0;
  virtual void OnClose() = 0;
  virtual void OnResize(int w, int h) = 0;
  virtual void OnMouseDown(int button, int x, int y) = 0;  // button 0 left, 1 right
  virtual void OnMouseUp(int button, int x, int y) = 0;
  virtual void OnMouseMove(int x, int y) = 0;
  virtual void OnLookDelta(float dx, float dy) = 0;  // mouse look (SetMouseLook): the move since the last event, pixels
  virtual void OnWheel(int ticks) = 0;               // + = away from the user
  virtual void OnKey(unsigned vk, bool repeat) = 0;
  virtual void OnFocusLost() = 0;
  virtual void OnCaptureLost() = 0;  // the mouse capture ended outside our control (Win32 WM_CAPTURECHANGED)
  // Session 98 phone port (stage C): touch gestures on the picture (platform_sdl.cpp; the mouse never sends these). Moves are in dp
  // (density-independent pixels: the screen's pixels / its UI scale), positions in pixels.
  virtual bool TouchOnUi(float ui_x, float ui_y) { (void)ui_x, (void)ui_y; return false; }  // a touch there belongs to the UI
  virtual void OnTouchLook(float dx, float dy) { (void)dx, (void)dy; }  // one finger moving
  virtual void OnTouchPan(float dx, float dy) { (void)dx, (void)dy; }   // two fingers: their centre moving
  virtual void OnTouchDolly(float d) { (void)d; }                       // two fingers: their spread changing (+ = apart)
  virtual void OnTap(int x, int y) { (void)x, (void)y; }                // one finger down and up without moving
  // Android: the window's surface goes away in the background and comes back in the foreground (renderer_vk.h SurfaceGone / Back).
  virtual void OnSurfaceGone() {}
  virtual void OnSurfaceBack() {}
};

// A Win32 window message as UiFirst receives it on Win32.
struct NativeMsg { void* hwnd; unsigned msg; uintptr_t wp; intptr_t lp; };

class Platform {
 public:
  virtual ~Platform() {}
  virtual const char* Name() const = 0;  // "win32" / "sdl"
  // The window, `w` x `h` at 96 dpi (scaled up on high-DPI screens). Call once.
  virtual bool CreateMainWindow(const char* title, int w, int h, std::string& err) = 0;
  virtual Surface MakeSurface() = 0;  // what the renderer draws into
  virtual void ClientSize(int& w, int& h) = 0;
  virtual void Pump(InputSink& sink) = 0;  // dispatch every pending event
  virtual bool Minimized() = 0;
  virtual bool Focused() = 0;               // the window has the keyboard
  virtual void SetTitle(const std::string& utf8) = 0;
  virtual bool CursorInClient(int& x, int& y) = 0;  // the cursor in client pixels; false when it is outside or the window has no focus
  virtual void SetMouseLook(bool on) = 0;  // cursor hidden and pinned; moves arrive as OnLookDelta
  virtual void SetCapture(bool on) = 0;    // keep the mouse while a button drags
  virtual bool KeyHeld(unsigned vk) = 0;   // a key (key:: / ASCII code) is down now
  virtual float PixelsPerDp() const { return 1.f; }  // touch moves (OnTouch*) arrive in dp: this many pixels each
  virtual void PollFreeKeys(FreeKeys& k, bool focused) = 0;  // the free camera's held keys
  virtual void PollPad(FreeCamCtl& c, bool focused) = 0;     // the first game pad into c.pad_*
  virtual void Destroy() = 0;
  // The Arena Viewer UI's backend for this window and renderer; null + err when the pair is not supported.
  virtual std::unique_ptr<ui::Backend> MakeUiBackend(Renderer* r, std::string& err) = 0;
};

std::unique_ptr<Platform> MakeWin32Platform();  // (Windows builds)
std::unique_ptr<Platform> MakeSdlPlatform();     // (builds with SDL3)

// The per-user data folder (Windows: %LOCALAPPDATA%; Android: the app's files folder, $WWE2K14_FILES_DIR set by ViewerActivity;
// elsewhere $HOME/.local/share or ".").
std::string LocalDataDir();

}  // namespace view
