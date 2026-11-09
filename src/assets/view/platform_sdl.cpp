// Session 98 (Arena Viewer phone port, stage B): the SDL3 window of the viewer (platform.h) - the Vulkan renderer's window on the PC
// (--backend vk) and the base of the phone's (stage C: touch gestures on top). Keys arrive as the Win32 numbers (platform.h key::).
#ifdef _WIN32
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "vk_loader.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "platform.h"
#include "renderer_vk.h"
#include "ui_backend_sdl3_vk.h"

namespace view {

namespace {

unsigned MapKey(SDL_Keycode k) {
  if (k >= SDLK_A && k <= SDLK_Z) return unsigned('A' + (k - SDLK_A));
  if (k >= SDLK_0 && k <= SDLK_9) return unsigned('0' + (k - SDLK_0));
  switch (k) {
    case SDLK_TAB: return key::kTab;
    case SDLK_ESCAPE: return key::kEscape;
    case SDLK_SPACE: return key::kSpace;
    case SDLK_PAGEUP: return key::kPrior;
    case SDLK_PAGEDOWN: return key::kNext;
    case SDLK_LEFT: return key::kLeft;
    case SDLK_UP: return key::kUp;
    case SDLK_RIGHT: return key::kRight;
    case SDLK_DOWN: return key::kDown;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return key::kShift;
    case SDLK_LCTRL: case SDLK_RCTRL: return key::kControl;
    case SDLK_LEFTBRACKET: return key::kOem4;
    case SDLK_RIGHTBRACKET: return key::kOem6;
    case SDLK_COMMA: return key::kComma;
    case SDLK_PERIOD: return key::kPeriod;
    case SDLK_MINUS: return key::kMinus;
    case SDLK_EQUALS: return key::kPlus;
    default: return 0;
  }
}

class SdlPlatform : public Platform {
 public:
  ~SdlPlatform() override { Destroy(); }
  const char* Name() const override { return "sdl"; }
  bool CreateMainWindow(const char* title, int w, int h, std::string& err) override {
#ifdef __ANDROID__
    // Session 98 phone port (stage C): landscape only (the sidebar layout), the whole screen (ViewerActivity reaches into the cut-out)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { err = std::string("SDL_Init: ") + SDL_GetError(); return false; }
    main_thread_ = SDL_GetCurrentThreadID();
    // Android: the surface must be dropped while SDL still runs this thread - with the default block-on-pause, SDL's event pump stops
    // inside the pause until the app comes back, so the queued background event would arrive only after the surface was gone.
    SDL_AddEventWatch(&SdlPlatform::Lifecycle, this);
    float ds = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());  // the window is w x h at 96 dpi, scaled up on high-DPI screens
    if (!(ds > 0.f)) ds = 1.f;
#ifdef _WIN32
    // (Windows: the surface is made from the window's HWND below - the SDK's static SDL3 is built without its Vulkan support)
    const SDL_WindowFlags vkflag = 0;
#elif defined(__ANDROID__)
    const SDL_WindowFlags vkflag = SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN;
#else
    const SDL_WindowFlags vkflag = SDL_WINDOW_VULKAN;
#endif
    win_ = SDL_CreateWindow(title, int(w * std::max(ds, 1.f)), int(h * std::max(ds, 1.f)), vkflag | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win_) { err = std::string("SDL_CreateWindow: ") + SDL_GetError(); return false; }
    return true;
  }
  Surface MakeSurface() override {
    Surface s;
#ifdef _WIN32
    s.vk_instance_exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    s.vk_create_surface = [hwnd](void* inst, uint64_t* out) {
      auto create = (PFN_vkCreateWin32SurfaceKHR)vkfn::GetProc("vkCreateWin32SurfaceKHR", inst);
      if (!create || !hwnd) return false;
      VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
      ci.hinstance = GetModuleHandleW(nullptr), ci.hwnd = hwnd;
      VkSurfaceKHR sf = VK_NULL_HANDLE;
      if (create((VkInstance)inst, &ci, nullptr, &sf) != VK_SUCCESS) return false;
      *out = (uint64_t)sf;
      return true;
    };
#else
    Uint32 n = 0;
    const char* const* ex = SDL_Vulkan_GetInstanceExtensions(&n);
    for (Uint32 i = 0; ex && i < n; i++) s.vk_instance_exts.push_back(ex[i]);
    SDL_Window* w = win_;
    s.vk_create_surface = [w](void* inst, uint64_t* out) {
      VkSurfaceKHR sf = VK_NULL_HANDLE;
      if (!SDL_Vulkan_CreateSurface(w, (VkInstance)inst, nullptr, &sf)) return false;
      *out = (uint64_t)sf;
      return true;
    };
#endif
    return s;
  }
  void ClientSize(int& w, int& h) override { SDL_GetWindowSizeInPixels(win_, &w, &h); }
  void Pump(InputSink& s) override {
    SDL_Event ev;
    const float px = SDL_GetWindowPixelDensity(win_) > 0.f ? SDL_GetWindowPixelDensity(win_) : 1.f;  // event coordinates -> pixels
    sink_ = &s;  // (the lifecycle watch runs inside SDL_PollEvent below)
    while (SDL_PollEvent(&ev)) {
      intptr_t out = 0;
      if (s.UiFirst(&ev, out)) continue;
      switch (ev.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: s.OnClose(); break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
          ResetTouch();
          if (!gone_) gone_ = true, s.OnSurfaceGone();  // (normally done already by the watch)
          break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
          if (gone_) gone_ = false, s.OnSurfaceBack();
          break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: Finger(s, ev); break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: s.OnResize(ev.window.data1, ev.window.data2); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: ResetTouch(); s.OnFocusLost(); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
          if (ev.button.which == SDL_TOUCH_MOUSEID) break;  // (a finger's copy as a mouse: the UI had it above; gestures come from the finger)
          const int b = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1 : -1;
          if (b < 0) break;
          const int x = int(ev.button.x * px), y = int(ev.button.y * px);
          if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) s.OnMouseDown(b, x, y);
          else s.OnMouseUp(b, x, y);
          break;
        }
        case SDL_EVENT_MOUSE_MOTION:
          if (ev.motion.which == SDL_TOUCH_MOUSEID) break;
          if (looking_) s.OnLookDelta(ev.motion.xrel * px, ev.motion.yrel * px);
          else s.OnMouseMove(int(ev.motion.x * px), int(ev.motion.y * px));
          break;
        case SDL_EVENT_MOUSE_WHEEL: {
          wheel_ += ev.wheel.y;
          const int t = int(wheel_);
          if (t) wheel_ -= float(t), s.OnWheel(t);
          break;
        }
        case SDL_EVENT_KEY_DOWN:
          if (const unsigned vk = MapKey(ev.key.key)) s.OnKey(vk, ev.key.repeat);
          break;
        default: break;
      }
    }
    sink_ = nullptr;
  }
  // (Android: no frames while the surface is gone)
  bool Minimized() override { return gone_ || (win_ && (SDL_GetWindowFlags(win_) & SDL_WINDOW_MINIMIZED)); }
  bool Focused() override { return win_ && (SDL_GetWindowFlags(win_) & SDL_WINDOW_INPUT_FOCUS); }
  void SetTitle(const std::string& t) override { if (win_) SDL_SetWindowTitle(win_, t.c_str()); }
  bool CursorInClient(int& x, int& y) override {
#ifdef __ANDROID__
    (void)x, (void)y;
    return false;  // (touch: no cursor, so no hover highlight; a tap selects)
#endif
    if (!win_) return false;
    float fx = 0, fy = 0;
    SDL_GetMouseState(&fx, &fy);  // (relative to the focused window)
    const float px = SDL_GetWindowPixelDensity(win_) > 0.f ? SDL_GetWindowPixelDensity(win_) : 1.f;
    x = int(fx * px), y = int(fy * px);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(win_, &w, &h);
    return SDL_GetMouseFocus() == win_ && Focused() && x >= 0 && y >= 0 && x < w && y < h;
  }
  void SetMouseLook(bool on) override {
    if (on == looking_ || !win_) return;
    looking_ = on;
    SDL_SetWindowRelativeMouseMode(win_, on);
  }
  void SetCapture(bool on) override { SDL_CaptureMouse(on); }
  bool KeyHeld(unsigned vk) override {
    if (vk == key::kShift) return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    if (vk == key::kControl) return (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    const bool* ks = SDL_GetKeyboardState(nullptr);
    if (vk >= 'A' && vk <= 'Z') return ks[SDL_SCANCODE_A + (vk - 'A')];
    if (vk >= '1' && vk <= '9') return ks[SDL_SCANCODE_1 + (vk - '1')];
    if (vk == '0') return ks[SDL_SCANCODE_0];
    return false;
  }
  // input_win32.h's PollFreeKeys / Pad, on SDL (the same keys, the same dead zones)
  void PollFreeKeys(FreeKeys& k, bool focused) override {
    auto down = [&](unsigned vk) { return focused && KeyHeld(vk); };
    k.fwd = down('W'), k.back = down('S'), k.left = down('A'), k.right = down('D');
    k.up = down('E'), k.down = down('Q');
    k.fast = down(key::kShift), k.fine = down(key::kControl);
    k.roll_left = down('Z'), k.roll_right = down('X'), k.roll_reset = down('R');
  }
  void PollPad(FreeCamCtl& c, bool focused) override {
    c.pad_move_x = c.pad_move_y = c.pad_look_x = c.pad_look_y = c.pad_up = c.pad_zoom = 0;
    if (!focused) return;
    if (!pad_) {
      int n = 0;
      SDL_JoystickID* ids = SDL_GetGamepads(&n);
      if (ids && n > 0) pad_ = SDL_OpenGamepad(ids[0]);
      SDL_free(ids);
      if (!pad_) return;
    }
    if (!SDL_GamepadConnected(pad_)) { SDL_CloseGamepad(pad_); pad_ = nullptr; return; }
    auto stick = [](Sint16 v) {  // dead zone 0.24 of full scale, then rescaled to 0..1
      const float x = float(v) / 32768.0f, a = std::fabs(x);
      if (a < 0.24f) return 0.f;
      return (x < 0 ? -1.0f : 1.0f) * std::min(1.0f, (a - 0.24f) / 0.76f);
    };
    auto trig = [](Sint16 v) { const float x = float(v) / 32767.f; return x < 0.12f ? 0.f : (x - 0.12f) / 0.88f; };
    c.pad_move_x = stick(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX));
    c.pad_move_y = -stick(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY));  // (SDL: down = +; XInput: up = +)
    c.pad_look_x = stick(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTX));
    c.pad_look_y = -stick(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTY));
    c.pad_up = trig(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) - trig(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    c.pad_zoom = float(SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) - float(SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
  }
  void Destroy() override {
    if (win_) SDL_RemoveEventWatch(&SdlPlatform::Lifecycle, this);
    if (pad_) SDL_CloseGamepad(pad_), pad_ = nullptr;
    if (win_) SDL_DestroyWindow(win_), win_ = nullptr;
  }
  std::unique_ptr<ui::Backend> MakeUiBackend(Renderer* r, std::string& err) override {
    auto* vk = dynamic_cast<RendererVk*>(r);
    if (!vk) { err = "the SDL window's UI needs the vk renderer"; return nullptr; }
    return ui::MakeSdl3VkBackend(win_, vk);
  }

 private:
  // Session 98 phone port (stage C): the background / foreground pair, on SDL's main thread while it pumps (see CreateMainWindow).
  static bool SDLCALL Lifecycle(void* ud, SDL_Event* ev) {
    auto* self = static_cast<SdlPlatform*>(ud);
    if ((ev->type == SDL_EVENT_WILL_ENTER_BACKGROUND || ev->type == SDL_EVENT_DID_ENTER_BACKGROUND) && self->sink_ && !self->gone_ &&
        SDL_GetCurrentThreadID() == self->main_thread_) {
      self->gone_ = true;
      self->ResetTouch();
      self->sink_->OnSurfaceGone();
    }
    return true;
  }

  // Session 98 phone port (stage C): touch on the picture (the design, out\runtime\s98\phone_port.md section 3): one finger looks,
  // two fingers pan (their centre) and pinch (their spread: forward / back), a short tap without moving selects. A touch that starts
  // on the UI (InputSink::TouchOnUi) is the UI's until it lifts: ImGui gets it as SDL's mouse copy of the finger (above).
  struct Touch { SDL_FingerID id; float x, y; };  // x, y in pixels
  std::vector<Touch> fingers_;                     // the camera's (started on the picture)
  std::vector<SDL_FingerID> ui_fingers_;           // the UI's
  bool tap_cand_ = false;
  float tap_x_ = 0, tap_y_ = 0;
  Uint64 tap_ms_ = 0;
  float span_ = 0, cx_ = 0, cy_ = 0;
  float Dp() const {  // pixels per dp (the phone launcher's UI scale: 3.5 on the Find X8 Ultra)
    const float s = SDL_GetWindowDisplayScale(win_);
    return s > 0.5f ? s : 1.f;
  }
 public:
  float PixelsPerDp() const override { return Dp(); }  // (Session 98: the importer's two-finger pan follows the fingers)
 private:
  // (Session 98 stage C review: fingers held when the app goes away never send their up: forget them all)
  void ResetTouch() {
    fingers_.clear(), ui_fingers_.clear();
    tap_cand_ = false;
  }
  void TwoFingerBase() {
    cx_ = (fingers_[0].x + fingers_[1].x) / 2, cy_ = (fingers_[0].y + fingers_[1].y) / 2;
    span_ = std::hypot(fingers_[0].x - fingers_[1].x, fingers_[0].y - fingers_[1].y);
  }
  void Finger(InputSink& s, const SDL_Event& ev) {
    int pw = 0, ph = 0, ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(win_, &pw, &ph);
    SDL_GetWindowSize(win_, &ww, &wh);
    const SDL_FingerID id = ev.tfinger.fingerID;
    const float x = ev.tfinger.x * float(pw), y = ev.tfinger.y * float(ph), dp = Dp();
    auto mine = std::find_if(fingers_.begin(), fingers_.end(), [&](const Touch& t) { return t.id == id; });
    auto ui = std::find(ui_fingers_.begin(), ui_fingers_.end(), id);
    switch (ev.type) {
      case SDL_EVENT_FINGER_DOWN:
        if (fingers_.empty() && (!ui_fingers_.empty() || s.TouchOnUi(ev.tfinger.x * float(ww), ev.tfinger.y * float(wh)))) {
          ui_fingers_.push_back(id);
          return;
        }
        if (fingers_.size() >= 2) return;  // (a third finger is ignored)
        fingers_.push_back({id, x, y});
        if (fingers_.size() == 1) tap_cand_ = true, tap_x_ = x, tap_y_ = y, tap_ms_ = SDL_GetTicks();
        else tap_cand_ = false, TwoFingerBase();
        return;
      case SDL_EVENT_FINGER_MOTION: {
        if (mine == fingers_.end()) return;
        const float dx = x - mine->x, dy = y - mine->y;
        mine->x = x, mine->y = y;
        if (fingers_.size() == 1) {
          if (tap_cand_ && std::hypot(x - tap_x_, y - tap_y_) > 8 * dp) tap_cand_ = false;
          if (!tap_cand_) s.OnTouchLook(dx / dp, dy / dp);
          return;
        }
        const float ocx = cx_, ocy = cy_, ospan = span_;
        TwoFingerBase();
        s.OnTouchPan((cx_ - ocx) / dp, (cy_ - ocy) / dp);
        s.OnTouchDolly((span_ - ospan) / dp);
        return;
      }
      default: {  // up / cancelled
        if (ui != ui_fingers_.end()) { ui_fingers_.erase(ui); return; }
        if (mine == fingers_.end()) return;
        const bool tap = fingers_.size() == 1 && tap_cand_ && ev.type == SDL_EVENT_FINGER_UP && SDL_GetTicks() - tap_ms_ < 500;
        fingers_.erase(mine);
        tap_cand_ = false;  // (one finger left after a pinch: it looks from here, no tap)
        if (tap) s.OnTap(int(tap_x_), int(tap_y_));
        return;
      }
    }
  }

  SDL_Window* win_ = nullptr;
  SDL_Gamepad* pad_ = nullptr;
  bool looking_ = false;
  float wheel_ = 0;
  InputSink* sink_ = nullptr;  // during Pump
  SDL_ThreadID main_thread_ = 0;
  bool gone_ = false;          // Android: the surface is away (background)
};

}  // namespace

std::unique_ptr<Platform> MakeSdlPlatform() { return std::make_unique<SdlPlatform>(); }

#ifndef _WIN32
std::string LocalDataDir() {
  if (const char* f = std::getenv("WWE2K14_FILES_DIR"); f && *f) return f;  // Android: the app's files folder (ViewerActivity)
  if (const char* x = std::getenv("XDG_DATA_HOME")) return x;
  if (const char* hm = std::getenv("HOME")) return std::string(hm) + "/.local/share";
  return ".";
}
#endif

}  // namespace view
