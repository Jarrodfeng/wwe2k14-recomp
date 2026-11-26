// wwe2k14 - RAMP CAMERA in the pause menu (S111 RAMPMENU, docs\research\s111\RAMPMENU.md; Jarrod 2026-11-26 21:3x: "the toggle for this
// I want to build into the pause menu's settings, as a sub option in VISUAL PRESENTATION, simply named "RAMP CAMERA" toggleable on and
// off"). The camera itself is lane RAMPCAM's (the switch wwe2k14_ramp_camera, read live every frame); this file is the row and its
// persistence. The pure rules are src\ramp_menu_core.h (tools\menus\ramp_menu_selftest.py).
//
//   - The row: page 3 (VISUAL PRESENTATION) of the in-match pause menu gets an 8th row after POST MATCH REPLAY (AddVpRow), from the
//     sub_82391928 hook in src\photo_mode.h when that call returns to page 3's filler (0x823853BC). The stock rows keep their places.
//   - A on it: the row's type is 5, as every stock row of the page (sub_823830D8 reads page 3's table at index 7), so the menu closes
//     and the pause flow (state 0, sub_8235DA28, page 3) hands action 7 to the page handler sub_8235C218. The stock handler takes
//     actions 0..6 only (7 would destroy the menu and make nothing, leaving the pause without a menu): our hook turns 7 into the game's
//     own ON / OFF question, the one CAMERA CUTS opens (action 0: sub_8238B5F0, flow state 4), opened on our value and retitled
//     RAMP CAMERA (sub_823A1E48 on the question: header VISUAL PRESENTATION, title ours). ON / OFF are the game's own words.
//   - The answer: flow state 4 sub_8235CBE0 (r4 = the row picked: 0 ON, 1 OFF) writes CAMERA CUTS' byte G+15204 and rebuilds page 3
//     with the cursor on CAMERA CUTS. For OUR question (the flow's current menu is the question we made) the game is handed
//     kAnswerNoWrite (its own "write nothing" case), our answer goes to wwe2k14_ramp_camera at once (the camera reacts in this match)
//     and to the launcher's settings file (key ramp_camera, the file the GRAPHICS menu writes), and the cursor goes to our row
//     (sub_82391100, the call the menu's constructor ends with).
//   - B in it: the flow's cancel step sub_8235BC90 (the question closed with no result, [menu+20] == 0) rebuilds the page the question
//     came from by its state (4 -> page 3 on CAMERA CUTS) and writes nothing; for our question the cursor is put back on RAMP CAMERA,
//     as the game does for each stock row's question.
//   - At start (ApplyAtStart, OnPreSetup after the replay's settings): ramp_menu_core.h StartDecision. Replay playback: the recording
//     decides. A value already given (the command line: the PC launcher passes true / false on every start but playback) wins. Else the
//     settings file the row writes (the --wwe2k14_settings_file path, else %APPDATA%\wwe2k14-launcher\settings.json on a PC,
//     <WWE2K14_FILES_DIR>/wwe2k14-launcher/settings.json on Android, src\platform\compat.h), in a release build, or with a settings file
//     given or use_launcher_settings on: a +dev build never applies the player's file by itself.
// Nothing is written into the game's files or save; CAMERA CUTS' byte is never written by the answer, and holds the game's own value
// again when the question hook's call returns (the question reads it to open on a row).
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "dev_build.h"  // wwe2k14::IsDevBuild (ApplyAtStart: CLAUDE.md's "a +dev build never applies the file by itself")
#include "gfx_menu.h"  // W, HostPtr, CallGuest, FlagIsTrue, SettingsFile, kTextRamp, g_ramp_pause_item; gfx_settings.h (ReadFile, GetBool, WriteJson)
#include "ramp_menu_core.h"

REX_EXTERN(__imp__sub_8235C218);
REX_EXTERN(__imp__sub_8235CBE0);
REX_EXTERN(__imp__sub_8235BC90);
REX_EXTERN(sub_823A1E48);  // the question's header / title texts (GetString -> +192 / +196)
REX_EXTERN(sub_82391100);  // a menu's selected row (the last call of the pause menu's constructor sub_82385598 +218)

namespace wwe2k14::ramp_menu {

constexpr const char* kFlag = "wwe2k14_ramp_camera";
inline uint32_t g_question = 0;  // the ON / OFF question we opened (a menu object), 0 = none

// The row exists only in an exe that has the camera's switch (lane RAMPCAM).
inline bool RowOn() {
  static const bool on = rex::cvar::GetFlagInfo(kFlag) != nullptr;
  return on;
}
inline bool RampOn() { return gfx_menu::FlagIsTrue(kFlag); }

// The launcher's settings file (the one the GRAPHICS menu writes; --wwe2k14_settings_file in tests): the key ramp_camera.
inline void Save(bool on) {
  const std::filesystem::path path = gfx_menu::SettingsFile();
  const bool ok = gfxs::WriteJson(path, SetBoolKey(gfxs::ReadFile(path), "ramp_camera", on));
  if (ok)
    REXLOG_INFO("[wwe2k14][ramp] RAMP CAMERA {} saved to {}", on ? "ON" : "OFF", gfxs::Utf8(path));
  else
    REXLOG_WARN("[wwe2k14][ramp] RAMP CAMERA {}: could not write {} (on for this run only)", on ? "ON" : "OFF", gfxs::Utf8(path));
}

// OnPreSetup, after the replay's settings (wwe2k14_app.h): the row's text is answered from now on; the switch's start value by
// StartDecision (ramp_menu_core.h). The file's value is set with SetFlagByName: every Want has already run (replay::PreSetup is before
// this call), and SetAutoFlag is not usable (the SDK's ResetAutoFlags at each Vulkan device creation, gpu_quirks_apply.cpp, would undo it).
inline void ApplyAtStart() {
  gfx_menu::g_ramp_pause_item = RowOn();
  if (!RowOn()) {
    REXLOG_INFO("[wwe2k14][ramp] no {} in this exe: no RAMP CAMERA row in the pause menu", kFlag);
    return;
  }
  RampStart s;
  s.replay_playback = !rex::cvar::GetFlagByName("wwe2k14_replay_play").empty();
  s.command_line = rex::cvar::GetFlagSource(kFlag) != rex::cvar::Source::kDefault;
  s.dev_build = wwe2k14::IsDevBuild();
  s.settings_file_given = !REXCVAR_GET(wwe2k14_settings_file).empty();
  s.use_launcher_settings = REXCVAR_GET(wwe2k14_use_launcher_settings);
  const std::filesystem::path path = gfx_menu::SettingsFile();
  bool on = false;
  if (StartDecisionRead(s, [&path] { return gfxs::ReadFile(path); }, on) != StartFrom::kFile) return;
  rex::cvar::SetFlagByName(kFlag, on ? "true" : "false");
  REXLOG_INFO("[wwe2k14][ramp] RAMP CAMERA {} (the settings file {})", on ? "ON" : "OFF", gfxs::Utf8(path));
}

// sub_82391928 from page 3's filler (the hook in src\photo_mode.h): r3 = the menu, r4 = its rows, r5 = the count.
inline void OnVpRows(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!RowOn() || ctx.r5.u32 != kStockRows) return;
  uint8_t* m = gfx_menu::HostPtr(ctx.r3.u32);
  const std::string why = StockVpWhy(m);
  if (!why.empty()) {
    static int refused = 0;
    if (refused++ < 3) REXLOG_WARN("[wwe2k14][ramp] pause menu {:08X} page 3 not as expected ({}): no RAMP CAMERA row", ctx.r3.u32, why);
    return;
  }
  AddVpRow(m, gfx_menu::kTextRamp);
  ctx.r5.u64 = kStockRows + 1;
  static int logged = 0;
  if (logged++ < 3) REXLOG_INFO("[wwe2k14][ramp] pause menu {:08X}: RAMP CAMERA row added after POST MATCH REPLAY", ctx.r3.u32);
}

}  // namespace wwe2k14::ramp_menu

// The VISUAL PRESENTATION page's handler (r3 = the pause flow, r4 = the action of the row picked). Every CAMERA CUTS question is made
// here (sub_8238B5F0's only callers), so the last question made is the one flow state 4 answers.
REX_HOOK_RAW(sub_8235C218) {
  using namespace wwe2k14::ramp_menu;
  g_question = 0;
  if (ctx.r4.u32 != kRampAction || !RowOn()) {
    __imp__sub_8235C218(ctx, base);
    return;
  }
  uint8_t* cuts = wwe2k14::gfx_menu::HostPtr(kCameraCutsByte);
  const uint8_t saved = *cuts;
  *cuts = QuestionByte(RampOn());  // the question opens on our value (the game's own rule: 1 -> row 0, ON)
  ctx.r4.u64 = 0;                  // CAMERA CUTS' branch: the game's ON / OFF question
  __imp__sub_8235C218(ctx, base);
  *cuts = saved;
  const uint32_t q = ctx.r3.u32;
  if (!q) {
    REXLOG_WARN("[wwe2k14][ramp] RAMP CAMERA: the game made no question (no memory?)");
    return;
  }
  wwe2k14::gfx_menu::CallGuest(ctx, base, sub_823A1E48, q, kVpHeaderText, wwe2k14::gfx_menu::kTextRamp);
  g_question = q;
  REXLOG_INFO("[wwe2k14][ramp] RAMP CAMERA picked: the ON / OFF question {:08X} (now {})", q, RampOn() ? "ON" : "OFF");
}

// Flow state 4 (r3 = the pause flow, its current menu at +104 = the question; r4 = the row picked). Ours: the game is handed its own
// no-write answer (it destroys the question and rebuilds page 3, CAMERA CUTS untouched), and the answer is ours.
REX_HOOK_RAW(sub_8235CBE0) {
  using namespace wwe2k14::ramp_menu;
  const bool ours = g_question && wwe2k14::gfx_menu::W(base, ctx.r3.u32 + 104) == g_question;
  g_question = 0;
  if (!ours) {
    __imp__sub_8235CBE0(ctx, base);
    return;
  }
  const uint32_t answer = ctx.r4.u32;
  ctx.r4.u64 = kAnswerNoWrite;
  __imp__sub_8235CBE0(ctx, base);
  const bool before = RampOn(), now = AnswerToValue(answer, before);
  REXLOG_INFO("[wwe2k14][ramp] RAMP CAMERA answer {}: {} -> {}", int32_t(answer), before ? "ON" : "OFF", now ? "ON" : "OFF");
  if (now != before) {
    rex::cvar::SetFlagByName(kFlag, now ? "true" : "false");
    Save(now);
  }
  const uint32_t page = ctx.r3.u32;  // the rebuilt page 3 (sub_82385598(menu, 3, 0, pad))
  if (page && HasRampRow(wwe2k14::gfx_menu::HostPtr(page), wwe2k14::gfx_menu::kTextRamp))
    wwe2k14::gfx_menu::CallGuest(ctx, base, sub_82391100, page, kRampAction);
}

// The flow's cancel step (r3 = the pause flow, its current menu at +104; reached when the menu closed with no result: B). For a CAMERA
// CUTS question (state 4) the game rebuilds page 3 with the cursor on CAMERA CUTS and writes nothing; for ours the cursor goes back to
// RAMP CAMERA. sub_8235BC90 stores the new page at [flow+104] itself and returns no menu in r3 (+314, +31C..+324).
REX_HOOK_RAW(sub_8235BC90) {
  using namespace wwe2k14::ramp_menu;
  using wwe2k14::gfx_menu::W;
  const uint32_t flow = ctx.r3.u32;
  const bool ours = g_question && W(base, flow + 104) == g_question;
  if (ours) g_question = 0;
  __imp__sub_8235BC90(ctx, base);
  if (!ours) return;
  const uint32_t page = W(base, flow + 104);
  REXLOG_INFO("[wwe2k14][ramp] RAMP CAMERA question left with B: still {}", RampOn() ? "ON" : "OFF");
  if (page && HasRampRow(wwe2k14::gfx_menu::HostPtr(page), wwe2k14::gfx_menu::kTextRamp))
    wwe2k14::gfx_menu::CallGuest(ctx, base, sub_82391100, page, kRampAction);
}
