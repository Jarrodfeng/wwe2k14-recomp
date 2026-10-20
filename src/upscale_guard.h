// Session 37: FSR never breaks a game (a playtester's Intel UHD 620 laptop shook and hung at a loading screen with
// FSR on, sound still playing).
//
// The renderer turns FSR off for the rest of the run when it fails (its runtime, its context or a dispatch; SDK
// rex::ui::vulkan::fsr::Fail), stops the scene jitter, shows the plain upscale and puts the reason in the SDK setting
// gpu_fsr_failed. Here, about once a second (a VdSwap hook; the cvar store has its own lock), the game picks that up:
//   - the log: "[wwe2k14][fsr] FSR turned off: <reason>";
//   - a notice on screen for 12 s (players too, not only harness runs);
//   - the launcher's settings file: upscaling off, so the next start does not try again, and "fsr_note" = the reason
//     (the launcher shows it under Graphics). Only when the file has an FSR mode saved (the player chose FSR there),
//     and in a +dev build only with --wwe2k14_settings_file (never Jarrod's own file from a test).
// Test without a weak GPU: --wwe2k14_fsr=true --gpu_fsr_test_fail=true (or `cvar gpu_fsr_test_fail true`).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "dlaa.h"              // wwe2k14_fsr
#include "gfx_menu.h"          // SettingsFile, IsDevBuild
#include "gfx_settings.h"      // the settings file's text helpers
#include "kernel_overrides.h"  // g_vdswap_hook

namespace wwe2k14::upscale_guard {

inline void (*g_prev_hook)(uint64_t frame) = nullptr;
inline std::atomic<bool> g_done{false};
inline rex::ui::ImGuiDrawer* g_drawer = nullptr;
inline std::function<void(std::function<void()>)> g_post_ui;
inline std::string g_text;  // set once before the notice is created (UI thread reads it afterwards)
inline std::chrono::steady_clock::time_point g_until;
inline rex::ui::ImGuiDialog* g_notice = nullptr;  // UI thread only

class Notice final : public rex::ui::ImGuiDialog {
 public:
  explicit Notice(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (std::chrono::steady_clock::now() >= g_until) {
      if (g_post_ui) g_post_ui([] {  // never delete a dialog from inside its own draw
          delete g_notice;
          g_notice = nullptr;
        });
      return;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 48.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##wwe2k14_fsr_notice", nullptr, flags)) {
      ImGui::SetWindowFontScale(1.4f);
      ImGui::PushTextWrapPos(io.DisplaySize.x * 0.6f);
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "AMD FSR could not run on this graphics card");
      ImGui::TextUnformatted(g_text.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
  }
};

// The settings file: upscaling off and the reason, when the player's own saved choice was FSR.
inline void SaveOff(const std::string& reason) {
  if (gfx_menu::IsDevBuild() && REXCVAR_GET(wwe2k14_settings_file).empty()) {
    REXLOG_INFO("[wwe2k14][fsr] settings file left alone (+dev build without --wwe2k14_settings_file)");
    return;
  }
  const auto path = gfx_menu::SettingsFile();
  std::string json = gfxs::ReadFile(path);
  int upscale = 0;
  if (json.empty() || !gfxs::GetInt(json, "upscale", upscale) || !gfxs::IsFsr(upscale)) {
    REXLOG_INFO("[wwe2k14][fsr] settings file has no FSR mode saved ({}): left alone", gfxs::Utf8(path));
    return;
  }
  json = gfxs::SetInt(json, "upscale", 0);
  json = gfxs::SetString(json, "fsr_note", reason);
  const bool ok = gfxs::WriteJson(path, json);
  REXLOG_INFO("[wwe2k14][fsr] settings file {}: upscaling {} (was FSR mode {})", gfxs::Utf8(path),
              ok ? "turned off" : "NOT changed (write failed)", upscale);
}

inline void React(const std::string& reason) {
  REXLOG_WARN("[wwe2k14][fsr] FSR turned off: {} (the game goes on with the plain upscale)", reason);
  g_text = "It has been turned off and the game carries on without it. Upscaling is now off in the launcher "
           "(Settings -> Graphics).\nReason: " + reason;
  g_until = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  if (g_post_ui && g_drawer)
    g_post_ui([] {
      if (!g_notice) g_notice = new Notice(g_drawer);
    });
  SaveOff(reason);
}

inline void OnSwap(uint64_t frame) {
  if (g_prev_hook) g_prev_hook(frame);
  if (g_done.load(std::memory_order_relaxed) || frame % 30) return;
  const std::string reason = rex::cvar::GetFlagByName("gpu_fsr_failed");
  if (reason.empty()) return;
  g_done = true;
  React(reason);
}

// From ReXApp::OnCreateDialogs (UI thread). Returns at once unless --wwe2k14_fsr=true.
inline void Install(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_ui) {
  if (!REXCVAR_GET(wwe2k14_fsr)) return;
  g_drawer = drawer;
  g_post_ui = std::move(post_ui);
  g_prev_hook = wwe2k14::g_vdswap_hook;
  wwe2k14::g_vdswap_hook = &OnSwap;
  REXLOG_INFO("[wwe2k14][fsr] guard armed: a failed FSR is turned off with a notice (src/upscale_guard.h)");
}

}  // namespace wwe2k14::upscale_guard
