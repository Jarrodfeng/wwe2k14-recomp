// wwe2k14 phone launcher - what the phone does for it (Session 71). One header, two implementations:
// platform_android.cpp (calls into LauncherActivity.java over JNI) and platform_desktop.cpp (the PC test build: the
// same launcher in a phone-shaped window, so it can be checked without a phone).
//
// Everything slow or owned by the system (file pickers, copying DLC, the share sheet, GitHub) answers later with an
// Event; the UI thread collects them with PollEvent once per frame.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../launcher/update_check.h"

namespace mlauncher {
// A path as UTF-8 in a std::string (C++20's u8string() is a std::u8string).
inline std::string U8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
}  // namespace mlauncher

namespace mlauncher::platform {

namespace fs = std::filesystem;

struct Event {
  enum Kind {
    kGamePicked,     // a = the address (content:// or a path), b = the name; ok = false: cancelled
    kDlcAdded,       // a = what happened (shown)
    kSavesExported,  // a = what happened
    kSavesImported,  // a = what happened
    kUpdate,         // update = GitHub's answer
    kBack,           // Android's Back gesture
    kMessage,        // a = text to show
  } kind = kMessage;
  bool ok = true;
  std::string a, b;
  launcher::UpdateInfo update;
};

// What the game's activity needs besides the command line (ANDROID_PLAN F1: the touch controls).
struct TouchChoice {
  int mode = 0, size = 100, opacity = 70;
  bool vibrate = false;
};

void Init(int argc, char** argv);
// The app's own folder (Android/data/<package>/files on the phone).
fs::path FilesDir();
// A name for the platform's log lines ("android" / "desktop").
const char* Name();

void PickGameFile();
// A path the launcher can read the picked game file through (the phone: /proc/self/fd/N of a file it opened from the
// address), or empty when it cannot be opened (moved, deleted, access lost).
std::string OpenGameFile(const std::string& address);
// Copies the files the player picks into `dlc_dir` (packs as they sit on the console's drive).
void AddDlc(const fs::path& dlc_dir);
// The system's share sheet with these files (logs).
void Share(const std::vector<fs::path>& files);
// A .zip of the save folder to a place the player picks / the save folder replaced from one (the old one kept as
// <save>.before_import).
void ExportSaves(const fs::path& save_dir);
void ImportSaves(const fs::path& save_dir);
void CheckForUpdate(const std::string& version);
void OpenUrl(const std::string& url);
// Starts the game with these arguments (game_address: the picked file, opened by the game's own process) and closes
// the launcher.
void StartGame(const std::vector<std::string>& args, const std::string& game_address, const TouchChoice& touch);
// Leaves the app (Back on the main page).
void Quit();
// The screen's size in pixels, landscape (width >= height).
void ScreenSize(int& w, int& h);

bool PollEvent(Event& e);
void Post(Event e);  // any thread

}  // namespace mlauncher::platform
