// wwe2k14 - on-screen checks with verdict keys (Session 29, test harness; nothing changes unless a script asks).
//
// Input-script command `check TEXT` (src/input_script.h) shows TEXT as a banner at the top of the game window and waits
// for Jarrod's answer on the keyboard: F1 = yes / looks right (PASS), F2 = no / wrong (FAIL), F5 = skip (SKIP).
// The answer is logged as
//   [wwe2k14][check] PASS line N after 4.2 s: TEXT
// and the script goes on (a FAIL does not stop it: the rest of the route may still be worth watching). No answer within
// --wwe2k14_check_timeout_s seconds (wall clock, default 300) logs TIMEOUT and goes on. tools/harness/run_verdict.py
// counts the verdicts; a live batch (src/live_control.h) returns them in its log lines.
// The banner is an ImGui overlay: it is not in screenshots or clips (they capture the guest output).
// The keys are only bound in harness runs (an input script or the live channel is set), never for players.
// Keys taken elsewhere: F3 debug overlay, F4 settings, F6 inspector, F7 achievements, F8 CineCam, F9 run-in.
// Session 30: test windows open without the focus and behind other windows (tools/bench/run_list.ps1), and the keys
// only reach a focused window: a question brings the window to the front WITH the focus (Jarrod: "draws focus when the
// question comes up"; --wwe2k14_check_focus, default on), so F1 / F2 / F5 work at once. Windows may refuse a background
// program the focus: then the window comes to the top without it and its taskbar button flashes (one click, then F1).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(wwe2k14_check_timeout_s, 300, "wwe2k14",
                     "Session 29: input-script `check TEXT`: seconds (wall clock) to wait for an answer key (F1 pass, "
                     "F2 fail, F5 skip) before logging TIMEOUT and going on");

REXCVAR_DEFINE_BOOL(wwe2k14_check_focus, true, "wwe2k14",
                    "Session 30: a `check` question brings the game window to the front with the keyboard focus (false: "
                    "to the top without the focus, taskbar button flashing)");

namespace wwe2k14::check {

enum Verdict : int { kNone = 0, kPass = 1, kFail = 2, kSkip = 3 };

inline const char* VerdictName(int v) {
  return v == kPass ? "PASS" : v == kFail ? "FAIL" : v == kSkip ? "SKIP" : "TIMEOUT";
}

class Banner;

struct State {
  std::mutex mutex;
  std::string text;       // the question on screen (guarded by mutex)
  int line = 0;
  std::atomic<bool> asking{false};
  std::atomic<int> verdict{kNone};
  rex::ui::ImGuiDrawer* drawer = nullptr;                        // UI thread only
  std::function<void(std::function<void()>)> post_ui;            // runs a function on the UI thread
  Banner* banner = nullptr;                                      // UI thread only
  void* hwnd = nullptr;                                          // the game window (Session 30: raise + flash)
  bool installed = false;
  bool binds = false;  // the F1 / F2 / F5 keys (harness runs only)
};

inline State& S() {
  static State s;
  return s;
}

class Banner final : public rex::ui::ImGuiDialog {
 public:
  explicit Banner(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    State& s = S();
    std::string text;
    int line = 0;
    {
      std::lock_guard lock(s.mutex);
      text = s.text;
      line = s.line;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 24.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(io.DisplaySize.x * 0.9f, io.DisplaySize.y * 0.5f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##wwe2k14_check", nullptr, flags)) {
      ImGui::SetWindowFontScale(1.7f);
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "CHECK (script line %d)", line);
      ImGui::PushTextWrapPos(io.DisplaySize.x * 0.85f);
      ImGui::TextUnformatted(text.c_str());
      ImGui::PopTextWrapPos();
      ImGui::Separator();
      ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "F1 = yes / looks right");
      ImGui::SameLine(0.0f, 40.0f);
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "F2 = no / wrong");
      ImGui::SameLine(0.0f, 40.0f);
      ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "F5 = skip");
    }
    ImGui::End();
  }
};

// Session 29: a short note at the top right (the bookmark key's "saved"), gone after a few seconds.
struct ToastState {
  std::mutex mutex;
  std::string text;
  std::chrono::steady_clock::time_point until;
  bool hide_posted = false;
  rex::ui::ImGuiDialog* dialog = nullptr;  // UI thread only
};
inline ToastState& T() {
  static ToastState t;
  return t;
}

inline void HideToast();

class Toast final : public rex::ui::ImGuiDialog {
 public:
  explicit Toast(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    ToastState& t = T();
    std::string text;
    bool expired = false;
    {
      std::lock_guard lock(t.mutex);
      text = t.text;
      expired = std::chrono::steady_clock::now() >= t.until;
      if (expired && !t.hide_posted && S().post_ui) {
        t.hide_posted = true;
        S().post_ui([] { HideToast(); });  // never delete a dialog from inside its own draw
      }
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 24.0f, 24.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##wwe2k14_toast", nullptr, flags)) {
      ImGui::SetWindowFontScale(1.5f);
      ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", text.c_str());
    }
    ImGui::End();
  }
};

inline void HideToast() {  // UI thread
  ToastState& t = T();
  delete t.dialog;
  t.dialog = nullptr;
}

// Any thread. No-op before Install (no overlay yet).
inline void ShowToast(const std::string& text, double seconds = 2.5) {
  State& s = S();
  if (!s.installed || !s.post_ui) return;
  {
    ToastState& t = T();
    std::lock_guard lock(t.mutex);
    t.text = text;
    t.until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(seconds * 1000));
    t.hide_posted = false;
  }
  s.post_ui([] {
    ToastState& t = T();
    if (!t.dialog && S().drawer) t.dialog = new Toast(S().drawer);
  });
}

inline void ShowBanner() {  // UI thread
  State& s = S();
  if (s.banner || !s.drawer) return;
  s.banner = new Banner(s.drawer);
#ifdef _WIN32
  // Session 30: bring the window to the front with the focus. A background program may not take the foreground by
  // itself (Windows' foreground lock); attaching to the foreground window's input queue for the call lets it, the
  // usual way. If Windows still refuses: on top without the focus, and the taskbar button flashes until it is in front.
  HWND hwnd = static_cast<HWND>(s.hwnd);
  if (!hwnd || GetForegroundWindow() == hwnd) return;
  bool focused = false;
  HWND fg = GetForegroundWindow();
  // Never take the focus from a program that does not respond: its deactivation would block our UI thread (review,
  // Session 30). Then: on top without the focus, flashing.
  if (REXCVAR_GET(wwe2k14_check_focus) && !(fg && IsHungAppWindow(fg))) {
    const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();
    const bool attached = fg_thread && fg_thread != me && AttachThreadInput(fg_thread, me, TRUE);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(fg_thread, me, FALSE);
    focused = GetForegroundWindow() == hwnd;
  } else {
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  if (!focused) {
    FLASHWINFO fi{sizeof(FLASHWINFO), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fi);
  }
  REXLOG_INFO("[wwe2k14][check] window brought forward: {}", focused ? "focused" : "on top, not focused (taskbar flashing)");
#endif
}

inline void HideBanner() {  // UI thread
  State& s = S();
  if (!s.banner) return;
  delete s.banner;  // removes it from the drawer
  s.banner = nullptr;
}

inline void Answer(int verdict) {  // UI thread (key bind)
  State& s = S();
  if (!s.asking.load()) return;
  int expected = kNone;
  s.verdict.compare_exchange_strong(expected, verdict);
}

// From ReXApp::OnCreateDialogs (UI thread). `harness` = an input script or the live channel is set: only then the
// keys are bound (players never get them). Session 58: the toast works for players too (F11's "marked" note).
inline void Install(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_ui, bool harness,
                    void* hwnd = nullptr) {
  if (!drawer) return;
  State& s = S();
  s.drawer = drawer;
  s.hwnd = hwnd;
  s.post_ui = std::move(post_ui);
  s.installed = true;
  if (!harness) return;
  s.binds = true;
  rex::ui::RegisterBind("bind_wwe2k14_check_pass", "F1", "Harness check: yes / looks right", [] { Answer(kPass); });
  rex::ui::RegisterBind("bind_wwe2k14_check_fail", "F2", "Harness check: no / wrong", [] { Answer(kFail); });
  rex::ui::RegisterBind("bind_wwe2k14_check_skip", "F5", "Harness check: skip", [] { Answer(kSkip); });
  REXLOG_INFO("[wwe2k14][check] installed: script `check TEXT` shows a question; F1 pass, F2 fail, F5 skip");
}

inline void Shutdown() {  // UI thread, before the drawer goes away
  State& s = S();
  if (!s.installed) return;
  if (s.binds) {
    rex::ui::UnregisterBind("bind_wwe2k14_check_pass");
    rex::ui::UnregisterBind("bind_wwe2k14_check_fail");
    rex::ui::UnregisterBind("bind_wwe2k14_check_skip");
  }
  HideBanner();
  HideToast();
  s.drawer = nullptr;
  s.post_ui = nullptr;
  s.hwnd = nullptr;
  s.installed = false;
}

// Game thread (input script). False when the overlay is not installed (the script then logs SKIP at once).
inline bool Ask(const std::string& text, int line) {
  State& s = S();
  if (!s.installed || !s.post_ui) return false;
  {
    std::lock_guard lock(s.mutex);
    s.text = text;
    s.line = line;
  }
  s.verdict.store(kNone);
  s.asking.store(true);
  s.post_ui([] { ShowBanner(); });
#ifdef _WIN32
  // Session 29: so Jarrod notices the question (asynchronous). "Default Beep": Windows 11's default scheme gives the
  // Question sound (MB_ICONQUESTION) no sound at all (Jarrod heard nothing, run 2185).
  MessageBeep(MB_OK);
#endif
  return true;
}

inline int Poll() { return S().verdict.load(); }

inline void Done() {
  State& s = S();
  if (!s.asking.exchange(false)) return;
  if (s.post_ui) s.post_ui([] { HideBanner(); });
}

}  // namespace wwe2k14::check
