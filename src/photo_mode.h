// wwe2k14 - photo mode (Session 33 item 4). Default off (--wwe2k14_photo_mode=true installs it).
//
// In a match: F10 on the keyboard, or L3 + R3 (both sticks clicked) held for half a second on the controller, enters
// and leaves it (B leaves too; neither key was used: F1-F9 and F11 are ours or the SDK's). Inside:
//   - the scene nearly stops: the game's step runs at 1000 Hz (--wwe2k14_step_hz, src\step_rate.h: 1 ms of game time
//     per frame, about 17x slower than normal; 1000 is the highest rate whose whole-millisecond field stays above 0).
//     The game's own pause cannot be used: it shows a blurred, tinted copy of the scene (run 2516, Session 33);
//   - the free camera (CineCam, src\cinecam.h): left stick move, right stick look, triggers up / down, bumpers zoom;
//     keyboard W/A/S/D + Q/E, right mouse button to look, wheel to zoom (CineCam's own keys; Shift fast, Ctrl fine);
//   - the HUD is hidden (SDK gpu_hide_hud: every draw after the post chain's last pass; CineCam's panel is hidden);
//   - depth of field on (CineCam's: the game's own depth-aware blur): X / Delete switches it on and off; focus follows
//     the nearest wrestler (the game's own focus distance) until D-pad up / down or PageUp / PageDown sets it by hand
//     (Y / Insert: back to automatic); D-pad left / right or Home / End open / close the aperture (a narrower or wider
//     sharp zone around the focus);
//   - A or Enter saves a PNG of the picture at the render resolution (the upscaler's output size with DLSS / FSR) to
//     Pictures\WWE 2K14 (--wwe2k14_photo_dir), named WWE2K14_YYYY-MM-DD_HH-MM-SS.png.
// While it is on the game gets no controller input (the pad drives the camera). Scripts: `cvar wwe2k14_photo_mode_on
// true|false`, `cvar wwe2k14_photo_save 1`.
//
// The pause menu's PHOTO MODE row (Session 33 follow-up, Jarrod's idea; --wwe2k14_photo_pause_item, on with photo
// mode). Found with the probe plugin, the call trace and coverage diffs (runs 025, coverage build):
//   - the in-match pause list is its own menu class (vtable 0x8200D898; not a SmartMenu). Its filler sub_82384FC8
//     (menu kind 3) writes each row's action into menu+196 + 4 i (0 RETURN TO GAME, 1 RESTART MATCH, 2 REPLAY,
//     3 VISUAL PRESENTATION, 4 CONTROLS, 5 MATCH INFORMATION, optional 6 / 7 / 9, last 8 QUIT MATCH) and the row texts
//     into the 20-byte records at menu+228 (+16 = text id), the count into menu+44, then hands them to the base
//     class's sub_82391928 (copied into the menu's display rows at [menu+40]). Every one of these holds 8 rows, and the
//     filler itself stops adding optional rows at 7: a normal match has 7, so there is room for exactly one more.
//   - A on a row runs the activate handler sub_823839D0 (r3 = menu + 28), which asks for the selected row's action
//     (vtable slot 11 = sub_82382F28: [menu+196 + 4 * [menu+48]]); a second reader (0x8235DC74) carries it out.
// So: in sub_82391928 called from that filler (return address 0x82385220) with the normal 7 rows (QUIT MATCH last),
// PHOTO MODE (action 10, our text) goes in before QUIT MATCH and the count becomes 8. When A is pressed on it, its
// action becomes 0 (RETURN TO GAME) before the game reads it: the game closes the pause its own way, and photo mode
// starts 40 frames later. The pause menu object is built afresh for every pause, so nothing has to be put back.
// The row table 0x8200D768 ({text id, type} per action) names the optional rows: 6 NEXT MATCH, 7 NEXT SHOW (Universe),
// 9 NEXT MOMENT (30 Years of WrestleMania; inferred from the texts). Those lists already have 8 rows. Jarrod
// (2026-10-19): Universe stays as it is (no PHOTO MODE row there; the keys work), and in the NEXT MOMENT list PHOTO
// MODE takes CONTROLS' place: RETURN TO GAME, RESTART MATCH, REPLAY, VISUAL PRESENTATION, MATCH INFORMATION,
// NEXT MOMENT, PHOTO MODE, QUIT MATCH.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "cinecam.h"
#include "gfx_menu.h"  // text ids, guest word helpers (the pause menu row)
#include "guest_read.h"
#include "input_script.h"  // SavePngWic, g_pad_filter

REXCVAR_DEFINE_BOOL(wwe2k14_photo_mode, false, "wwe2k14",
                    "Session 33: photo mode in a match (F10, or L3 + R3 held on the controller): the scene nearly "
                    "stops, a free camera, no HUD, depth of field, A / Enter saves a PNG to Pictures\\WWE 2K14. Turns "
                    "the CineCam on. Default off. See src/photo_mode.h");
REXCVAR_DEFINE_STRING(wwe2k14_photo_dir, "", "wwe2k14",
                      "Session 33: where photo mode saves its pictures; empty = Pictures\\WWE 2K14");
REXCVAR_DEFINE_BOOL(wwe2k14_photo_mode_on, false, "wwe2k14",
                    "Session 33 (scripts): true / false enters / leaves photo mode (in a match)");
REXCVAR_DEFINE_INT32(wwe2k14_photo_save, 0, "wwe2k14", "Session 33 (scripts): set to 1 to save a photo (photo mode on)");
REXCVAR_DEFINE_BOOL(wwe2k14_photo_pause_test_moment, false, "wwe2k14",
                    "Session 33 (test): make a normal match's pause list look like 30 Years of WrestleMania's (NEXT "
                    "MOMENT before QUIT MATCH) before the PHOTO MODE row is placed; do not pick NEXT MOMENT then");
REXCVAR_DEFINE_BOOL(wwe2k14_photo_pause_item, true, "wwe2k14",
                    "Session 33: with --wwe2k14_photo_mode, a PHOTO MODE row in the match's pause menu (before QUIT "
                    "MATCH); false = the keys only");

REX_EXTERN(__imp__sub_82391928);
REX_EXTERN(__imp__sub_823839D0);

namespace wwe2k14::photo {

inline std::atomic<bool> g_active{false};
inline std::atomic<bool> g_toggle_request{false};
inline std::atomic<bool> g_save_request{false};
inline std::string g_saved_step_hz, g_saved_panel, g_saved_step_vblank;
inline int g_saved_dof_mode = 0;
inline float g_aperture = 1.0f;  // scales CineCam's in-focus distances (smaller = a narrower sharp zone)
inline std::atomic<uint64_t> g_enter_at{0};  // PHOTO MODE picked in the pause menu: enter at this guest frame

constexpr uint32_t kPauseMenuVtable = 0x8200D898;
constexpr uint32_t kPhotoAction = 10;   // not one of the game's (0-9)
constexpr uint32_t kQuitMatchAction = 8;
constexpr uint32_t kQuitMatchText = 0x94B;
constexpr uint32_t kControlsAction = 4, kControlsText = 0x9EB;
constexpr uint32_t kNextMomentAction = 9, kNextMomentText = 0x9EE;

inline bool PauseItemOn() { return REXCVAR_GET(wwe2k14_photo_mode) && REXCVAR_GET(wwe2k14_photo_pause_item); }

inline uint32_t RowWord(uint8_t* base, uint32_t menu, int row, int k) { return gfx_menu::W(base, menu + 228 + 20 * row + 4 * k); }
inline void CopyRow(uint8_t* base, uint32_t menu, int from, int to) {
  for (int k = 0; k < 5; ++k) gfx_menu::Put32(menu + 228 + 20 * to + 4 * k, RowWord(base, menu, from, k));
  gfx_menu::Put32(menu + 196 + 4 * to, gfx_menu::W(base, menu + 196 + 4 * from));
}
inline void PutPhotoRow(uint32_t menu, int row) {
  for (int k = 0; k < 4; ++k) gfx_menu::Put32(menu + 228 + 20 * row + 4 * k, 0);
  gfx_menu::Put32(menu + 228 + 20 * row + 16, gfx_menu::kTextPhoto);
  gfx_menu::Put32(menu + 196 + 4 * row, kPhotoAction);
}

// The 30 Years of WrestleMania list (8 rows, NEXT MOMENT before QUIT MATCH): PHOTO MODE takes CONTROLS' place (see
// the top of this file). Universe's 8-row lists (NEXT MATCH / NEXT SHOW) are left alone.
inline bool SwapControlsRow(uint8_t* base, uint32_t menu) {
  using gfx_menu::W;
  if (W(base, menu) != kPauseMenuVtable || W(base, menu + 44) != 8 || W(base, menu + 196 + 4 * 4) != kControlsAction ||
      RowWord(base, menu, 4, 4) != kControlsText || W(base, menu + 196 + 4 * 6) != kNextMomentAction ||
      RowWord(base, menu, 6, 4) != kNextMomentText || W(base, menu + 196 + 4 * 7) != kQuitMatchAction)
    return false;  // not the NEXT MOMENT list (Universe, or another layout): no PHOTO MODE row
  CopyRow(base, menu, 5, 4);  // MATCH INFORMATION up
  CopyRow(base, menu, 6, 5);  // NEXT MOMENT up
  PutPhotoRow(menu, 6);       // PHOTO MODE before QUIT MATCH (row 7)
  static int logged = 0;
  if (logged++ < 3) REXLOG_INFO("[wwe2k14][photo] pause menu {:08X} (NEXT MOMENT list): PHOTO MODE in CONTROLS' place", menu);
  return true;
}

// Test: a normal 7-row list made into the NEXT MOMENT one (--wwe2k14_photo_pause_test_moment).
inline bool FakeMomentRow(uint8_t* base, uint32_t menu) {
  using gfx_menu::W;
  if (W(base, menu) != kPauseMenuVtable || W(base, menu + 44) != 7 || W(base, menu + 196 + 4 * 6) != kQuitMatchAction)
    return false;
  CopyRow(base, menu, 6, 7);  // QUIT MATCH down
  for (int k = 0; k < 4; ++k) gfx_menu::Put32(menu + 228 + 20 * 6 + 4 * k, 0);
  gfx_menu::Put32(menu + 228 + 20 * 6 + 16, kNextMomentText);
  gfx_menu::Put32(menu + 196 + 4 * 6, kNextMomentAction);
  gfx_menu::Put32(menu + 44, 8);
  REXLOG_INFO("[wwe2k14][photo] test: pause menu {:08X} given a NEXT MOMENT row (8 rows)", menu);
  return true;
}

// sub_82391928 from the in-match filler: the 8th row (see the top of this file). The rows are the menu's own.
inline bool AddPauseRow(uint8_t* base, uint32_t menu) {
  using gfx_menu::W;
  using gfx_menu::Put32;
  if (W(base, menu) != kPauseMenuVtable || W(base, menu + 44) != 7 || W(base, menu + 196 + 4 * 6) != kQuitMatchAction ||
      W(base, menu + 228 + 20 * 6 + 16) != kQuitMatchText) {
    static int refused = 0;
    if (refused++ < 3)
      REXLOG_WARN("[wwe2k14][photo] pause menu {:08X} not as expected (vtable {:08X}, rows {}, last action {}, last "
                  "text 0x{:X}): no PHOTO MODE row", menu, W(base, menu), W(base, menu + 44),
                  W(base, menu + 196 + 4 * 6), W(base, menu + 228 + 20 * 6 + 16));
    return false;
  }
  for (int k = 0; k < 5; ++k) Put32(menu + 228 + 20 * 7 + 4 * k, W(base, menu + 228 + 20 * 6 + 4 * k));  // QUIT down
  for (int k = 0; k < 4; ++k) Put32(menu + 228 + 20 * 6 + 4 * k, 0);
  Put32(menu + 228 + 20 * 6 + 16, gfx_menu::kTextPhoto);
  Put32(menu + 196 + 4 * 7, kQuitMatchAction);
  Put32(menu + 196 + 4 * 6, kPhotoAction);
  Put32(menu + 44, 8);
  static int logged = 0;
  if (logged++ < 3) REXLOG_INFO("[wwe2k14][photo] pause menu {:08X}: PHOTO MODE row added before QUIT MATCH", menu);
  return true;
}

inline std::string Utf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

inline std::filesystem::path PhotoDir() {
  const std::string d = REXCVAR_GET(wwe2k14_photo_dir);
  if (!d.empty()) return std::filesystem::u8path(d);
  PWSTR pics = nullptr;
  std::filesystem::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) dir = pics;
  CoTaskMemFree(pics);
  if (dir.empty()) dir = std::filesystem::current_path();
  return dir / L"WWE 2K14";
}

inline void SavePhoto() {
  std::thread([] {
    auto* runtime = REX_KERNEL_STATE()->emulator();
    auto* graphics = runtime ? runtime->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
      REXLOG_WARN("[wwe2k14][photo] no picture to save");
      return;
    }
    const auto dir = PhotoDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char name[64];
    std::strftime(name, sizeof(name), "WWE2K14_%Y-%m-%d_%H-%M-%S", &tm);
    auto path = dir / (std::string(name) + ".png");
    for (int i = 2; std::filesystem::exists(path, ec) && i < 100; ++i)
      path = dir / (std::string(name) + "_" + std::to_string(i) + ".png");
    auto part = path;
    part += ".part";
    if (SavePngWic(part, image) && (std::filesystem::rename(part, path, ec), !ec))
      REXLOG_INFO("[wwe2k14][photo] saved {} ({}x{})", Utf8(path), image.width, image.height);
    else
      REXLOG_WARN("[wwe2k14][photo] could not write {} ({})", Utf8(path), ec ? ec.message() : "WIC");
  }).detach();
}

inline bool InMatch(uint8_t* base) {
  auto rd = [base](uint32_t a, uint32_t* out) {
    if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
    *out = _byteswap_ulong(v);
    return true;
  };
  uint32_t a = 0, b = 0, seq = 0;  // states.toml seq: 9 = in game
  return rd(0x839400F8, &a) && rd(a, &b) && rd(b + 48, &seq) && seq == 9;
}

inline void Enter() {
  g_saved_step_hz = rex::cvar::GetFlagByName("wwe2k14_step_hz");
  g_saved_panel = rex::cvar::GetFlagByName("wwe2k14_cinecam_panel");
  // High refresh (--wwe2k14_step_vblank) would make the vblank follow the 1000 Hz step: off while in photo mode.
  g_saved_step_vblank = rex::cvar::GetFlagByName("wwe2k14_step_vblank");
  rex::cvar::SetFlagByName("wwe2k14_step_vblank", "false");
  rex::cvar::SetFlagByName("wwe2k14_step_hz", "1000");
  rex::cvar::SetFlagByName("wwe2k14_cinecam_panel", "false");
  rex::cvar::SetFlagByName("gpu_hide_hud", "true");
  {
    cinecam::FreeCam& f = cinecam::F();
    std::lock_guard lock(f.mutex);
    g_saved_dof_mode = f.dof_mode;
    f.dof_mode = 2, f.dof_manual = false;
  }
  rex::cvar::SetFlagByName("wwe2k14_cinecam_on", "true");
  g_active = true;
  REXLOG_INFO("[wwe2k14][photo] photo mode on (step 1000 Hz, free camera, HUD hidden, depth of field on)");
}

inline void Leave() {
  g_active = false;
  rex::cvar::SetFlagByName("wwe2k14_cinecam_on", "false");
  rex::cvar::SetFlagByName("gpu_hide_hud", "false");
  rex::cvar::SetFlagByName("wwe2k14_step_hz", g_saved_step_hz.empty() ? "0" : g_saved_step_hz);
  rex::cvar::SetFlagByName("wwe2k14_step_vblank", g_saved_step_vblank.empty() ? "false" : g_saved_step_vblank);
  rex::cvar::SetFlagByName("wwe2k14_cinecam_panel", g_saved_panel.empty() ? "true" : g_saved_panel);
  {
    cinecam::FreeCam& f = cinecam::F();
    std::lock_guard lock(f.mutex);
    f.dof_mode = g_saved_dof_mode;
    f.pad_move_x = f.pad_move_y = f.pad_look_x = f.pad_look_y = f.pad_up = f.pad_zoom = 0;
  }
  REXLOG_INFO("[wwe2k14][photo] photo mode off");
}

// Focus / aperture steps (pad or keys); manual focus starts from the automatic one.
inline void Focus(float factor) {
  cinecam::FreeCam& f = cinecam::F();
  std::lock_guard lock(f.mutex);
  if (!f.dof_manual) f.dof_manual = true, f.dof_focus = f.dof_auto_focus > 0 ? f.dof_auto_focus : f.dof_focus;
  f.dof_focus = std::clamp(f.dof_focus * factor, 50.0f, 50000.0f);
}
inline void Aperture(float factor) {
  cinecam::FreeCam& f = cinecam::F();
  std::lock_guard lock(f.mutex);
  g_aperture = std::clamp(g_aperture * factor, 0.1f, 10.0f);
  for (int i = 0; i < 4; ++i) f.dof_dist[i] = cinecam::kDofDefaultDist[i] * g_aperture;
}
inline void ToggleDof() {
  cinecam::FreeCam& f = cinecam::F();
  std::lock_guard lock(f.mutex);
  f.dof_mode = f.dof_mode == 2 ? 1 : 2;
  REXLOG_INFO("[wwe2k14][photo] depth of field {}", f.dof_mode == 2 ? "on" : "off");
}
inline void AutoFocus() {
  cinecam::FreeCam& f = cinecam::F();
  std::lock_guard lock(f.mutex);
  f.dof_manual = false;
}

// The controller (XamInputGetState, user 1; the game's thread): the L3 + R3 hold toggles; while on, the pad drives
// the camera and the game sees an idle pad.
inline void PadFilter(rex::input::X_INPUT_STATE* s) {
  static auto combo_since = std::chrono::steady_clock::time_point{};
  static bool combo_fired = false;
  static uint16_t prev = 0;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
  last = now;
  const uint16_t b = uint16_t(s->gamepad.buttons);
  const bool combo = (b & 0x00C0) == 0x00C0;  // L3 + R3
  if (combo) {
    if (combo_since == std::chrono::steady_clock::time_point{}) combo_since = now;
    if (!combo_fired && now - combo_since > std::chrono::milliseconds(400)) combo_fired = true, g_toggle_request = true;
  } else {
    combo_since = {}, combo_fired = false;
  }
  if (!g_active) {
    prev = b;
    return;
  }
  const uint16_t down = uint16_t(b & ~prev);
  prev = b;
  if (down & 0x1000) g_save_request = true;     // A
  if (down & 0x2000) g_toggle_request = true;   // B: leave
  if (down & 0x4000) ToggleDof();               // X
  if (down & 0x8000) AutoFocus();               // Y
  if (b & 0x0001) Focus(std::pow(0.5f, dt));    // D-pad up: nearer (half per second)
  if (b & 0x0002) Focus(std::pow(2.0f, dt));    // D-pad down: further
  if (b & 0x0004) Aperture(std::pow(0.5f, dt)); // D-pad left: narrower sharp zone (more blur)
  if (b & 0x0008) Aperture(std::pow(2.0f, dt)); // D-pad right: wider
  auto axis = [](int v) {
    float x = float(v) / 32767.0f;
    const float dz = 0.2f;
    if (std::fabs(x) < dz) return 0.0f;
    return std::clamp((std::fabs(x) - dz) / (1 - dz), 0.0f, 1.0f) * (x < 0 ? -1.0f : 1.0f);
  };
  {
    cinecam::FreeCam& f = cinecam::F();
    std::lock_guard lock(f.mutex);
    f.pad_move_x = axis(int16_t(s->gamepad.thumb_lx));
    f.pad_move_y = axis(int16_t(s->gamepad.thumb_ly));
    f.pad_look_x = axis(int16_t(s->gamepad.thumb_rx));
    f.pad_look_y = axis(int16_t(s->gamepad.thumb_ry));
    f.pad_up = (float(s->gamepad.right_trigger) - float(s->gamepad.left_trigger)) / 255.0f;
    f.pad_zoom = float(!!(b & 0x0200)) - float(!!(b & 0x0100));  // RB in, LB out
  }
  s->gamepad.buttons = 0;
  s->gamepad.left_trigger = 0, s->gamepad.right_trigger = 0;
  s->gamepad.thumb_lx = 0, s->gamepad.thumb_ly = 0, s->gamepad.thumb_rx = 0, s->gamepad.thumb_ry = 0;
}

inline void OnFrame(uint8_t* base);

// From ReXApp::OnCreateDialogs (UI thread): the keys.
inline void Install() {
  if (!REXCVAR_GET(wwe2k14_photo_mode)) return;
  rex::ui::RegisterBind("bind_wwe2k14_photo", "F10", "Photo mode on / off (in a match)", [] { g_toggle_request = true; });
  rex::ui::RegisterBind("bind_wwe2k14_photo_save", "Return", "Photo mode: save a photo", [] {
    if (g_active) g_save_request = true;
  });
  rex::ui::RegisterBind("bind_wwe2k14_photo_focus_near", "PageUp", "Photo mode: focus nearer", [] {
    if (g_active) Focus(0.85f);
  });
  rex::ui::RegisterBind("bind_wwe2k14_photo_focus_far", "PageDown", "Photo mode: focus further", [] {
    if (g_active) Focus(1.0f / 0.85f);
  });
  rex::ui::RegisterBind("bind_wwe2k14_photo_aperture_open", "Home", "Photo mode: narrower sharp zone", [] {
    if (g_active) Aperture(0.8f);
  });
  rex::ui::RegisterBind("bind_wwe2k14_photo_aperture_close", "End", "Photo mode: wider sharp zone", [] {
    if (g_active) Aperture(1.25f);
  });
  rex::ui::RegisterBind("bind_wwe2k14_photo_autofocus", "Insert", "Photo mode: automatic focus", [] {
    if (g_active) AutoFocus();
  });
  rex::ui::RegisterBind("bind_wwe2k14_photo_dof", "Delete", "Photo mode: depth of field on / off", [] {
    if (g_active) ToggleDof();
  });
  g_pad_filter = PadFilter;
  gfx_menu::g_photo_pause_item = PauseItemOn();
  g_photo_frame_hook = OnFrame;
  REXLOG_INFO("[wwe2k14][photo] installed: F10 or L3 + R3 (held) in a match; photos go to {}", Utf8(PhotoDir()));
}

// The game's frame (VdSwap): requests become changes here.
inline void OnFrame(uint8_t* base) {
  if (!REXCVAR_GET(wwe2k14_photo_mode)) return;
  if (const uint64_t at = g_enter_at.load(); at && wwe2k14::g_guest_frame.load() >= at) {
    g_enter_at = 0;
    if (!g_active) g_toggle_request = true;
  }
  if (REXCVAR_GET(wwe2k14_photo_mode_on) != g_active.load()) g_toggle_request = true;
  if (REXCVAR_GET(wwe2k14_photo_save)) {
    rex::cvar::SetFlagByName("wwe2k14_photo_save", "0");
    if (g_active) g_save_request = true;
  }
  if (g_toggle_request.exchange(false)) {
    if (g_active) {
      Leave();
      rex::cvar::SetFlagByName("wwe2k14_photo_mode_on", "false");
    } else if (InMatch(base)) {
      Enter();
      rex::cvar::SetFlagByName("wwe2k14_photo_mode_on", "true");
    } else {
      REXLOG_INFO("[wwe2k14][photo] photo mode is for matches (not in one now)");
      rex::cvar::SetFlagByName("wwe2k14_photo_mode_on", "false");
    }
  }
  if (g_active && !InMatch(base)) {  // the match ended under us (quit from the menu, a load)
    Leave();
    rex::cvar::SetFlagByName("wwe2k14_photo_mode_on", "false");
  }
  if (g_save_request.exchange(false) && g_active) SavePhoto();
}

}  // namespace wwe2k14::photo

// The pause menu's rows are handed to the base class (see the top of this file).
REX_HOOK_RAW(sub_82391928) {
  if (wwe2k14::photo::PauseItemOn() && uint32_t(ctx.lr) == 0x82385220) {
    using namespace wwe2k14::photo;
    if (ctx.r5.u32 == 7 && REXCVAR_GET(wwe2k14_photo_pause_test_moment) && FakeMomentRow(base, ctx.r3.u32))
      ctx.r5.u64 = 8;
    if (ctx.r5.u32 == 7 && AddPauseRow(base, ctx.r3.u32)) ctx.r5.u64 = 8;
    else if (ctx.r5.u32 == 8) SwapControlsRow(base, ctx.r3.u32);
  }
  __imp__sub_82391928(ctx, base);
}

// A on a pause row: PHOTO MODE becomes RETURN TO GAME for the game, and photo mode follows.
REX_HOOK_RAW(sub_823839D0) {
  using wwe2k14::gfx_menu::W;
  if (wwe2k14::photo::PauseItemOn()) {
    const uint32_t menu = ctx.r3.u32 - 28;
    if (ctx.r4.u32 == W(base, ctx.r3.u32 + 8) && W(base, menu) == wwe2k14::photo::kPauseMenuVtable) {
      const uint32_t sel = W(base, menu + 48), count = W(base, menu + 44);
      if (sel < count && sel < 8 && W(base, menu + 196 + 4 * sel) == wwe2k14::photo::kPhotoAction) {
        wwe2k14::gfx_menu::Put32(menu + 196 + 4 * sel, 0);
        wwe2k14::photo::g_enter_at = wwe2k14::g_guest_frame.load() + 40;
        REXLOG_INFO("[wwe2k14][photo] PHOTO MODE picked in the pause menu: the game resumes, photo mode in 40 frames");
      }
    }
  }
  __imp__sub_823839D0(ctx, base);
}
