// Session 53 item 2.1 (a playtester: "--game_data_root was not provided" when wwe2k14.exe is started directly, by a
// double-click or a shortcut). The game only gets --game_data_root from the launcher's command line; a release
// started without it now:
//   1. with no game flags at all (only a test's --wwe2k14_settings_file) and the launcher next to it: hands over to
//      the launcher (`wwe2k14_launcher.exe --start-game`), which starts the game with the player's own Play settings
//      (game files, save folder, mods, keyboard, language, window, graphics), exactly as PLAY GAME does, without
//      showing its own window unless something needs the player (no game files yet, a crash);
//   2. else: takes the game files and save folders from the launcher's settings file (the file it already reads for
//      the graphics settings; --wwe2k14_settings_file in tests);
//   3. else: a message that says where to pick the game files (instead of the SDK's "--game_data_root was not
//      provided"), and exit code 1 (the launcher's "the game reported an error itself").
// Development builds (+dev) are unchanged: they find game_data\extracted above the exe (OnConfigurePaths).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <shellapi.h>
#endif

#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>

#include "gfx_menu.h"      // IsDevBuild, SettingsFile
#include "gfx_settings.h"  // ReadFile, GetString

REXCVAR_DEFINE_BOOL(wwe2k14_direct_handoff, true, "wwe2k14",
                    "Session 53: a release started with no game flags (a double-click, a shortcut) hands over to the "
                    "launcher next to it (wwe2k14_launcher.exe --start-game: the player's Play settings, its window "
                    "hidden). =false takes only the game files and save folders from the launcher's settings file");

namespace wwe2k14::direct_start {

namespace fs = std::filesystem;

inline fs::path FromUtf8(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

#if !defined(_WIN32)
// Session 63 (Android port): no launcher exe to hand over to; the app's own start screen comes with ANDROID_PLAN F4.
inline bool NoGameFlags() { return false; }
inline bool HandOffToLauncher() { return false; }
#else
// The command line has no game flags: nothing, or only the flags a test adds (its settings file, this switch).
inline bool NoGameFlags() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool none = true;
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a.rfind(L"--wwe2k14_settings_file=", 0) != 0 && a.rfind(L"--wwe2k14_direct_handoff", 0) != 0) none = false;
  }
  if (argv) LocalFree(argv);
  return none;
}

// 1. The launcher next to the exe starts the game the way PLAY GAME does. True when it was started.
inline bool HandOffToLauncher() {
  const fs::path launcher = rex::filesystem::GetExecutableFolder() / "wwe2k14_launcher.exe";
  std::error_code ec;
  if (!fs::is_regular_file(launcher, ec)) return false;
  std::wstring cmd = L"\"" + launcher.wstring() + L"\" --start-game";
  const std::string file = REXCVAR_GET(wwe2k14_settings_file);
  if (!file.empty()) cmd += L" --settings \"" + FromUtf8(file).wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const std::wstring dir = launcher.parent_path().wstring();
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}
#endif  // _WIN32

// Called at the end of OnConfigurePaths when no game files were found (no --game_data_root, no game_data\extracted
// above the exe). Logging is not set up yet, so nothing here logs; OnPreSetup's "[wwe2k14] game_data_root" line shows
// the result.
inline void Configure(rex::PathConfig& paths) {
  if (gfx_menu::IsDevBuild() || !paths.game_data_root.empty()) return;
  if (REXCVAR_GET(wwe2k14_direct_handoff) && NoGameFlags() && HandOffToLauncher()) ExitProcess(0);
  // 2. The launcher's settings file.
  const std::string json = gfxs::ReadFile(gfx_menu::SettingsFile());
  std::string game, user;
  gfxs::GetString(json, "game_data_dir", game);
  gfxs::GetString(json, "user_data_dir", user);
  if (!game.empty()) {
    paths.game_data_root = FromUtf8(game);
    if (!user.empty() && rex::cvar::GetFlagSource("user_data_root") == rex::cvar::Source::kDefault) {
      const fs::path old_cache = paths.user_data_root / "cache";
      paths.user_data_root = FromUtf8(user);
      if (paths.cache_root == old_cache) paths.cache_root = paths.user_data_root / "cache";  // the SDK's default
    }
    return;
  }
  // 3. Nothing: say where to pick the game files.
  MessageBoxW(nullptr,
              L"WWE 2K14 can't find your game files.\n\n"
              L"Open wwe2k14_launcher.exe (in the same folder as the game), pick your game files in "
              L"Settings -> Game data, then press PLAY GAME there.",
              L"WWE 2K14", MB_ICONERROR | MB_OK);
  ExitProcess(1);  // the launcher's kGameReportedErrorExitCode: the game showed its own message
}

}  // namespace wwe2k14::direct_start
