// wwe2k14 phone launcher - the PC test build's platform (Session 71). The launcher runs in a phone-shaped window; what
// only a phone can do (the share sheet, save export) says so instead, and PLAY writes the game's command line to
// <files>/last_launch.txt rather than starting anything, so a test can read what the phone's game would get.
//   --files DIR       the app folder stand-in (default: out\launcher_mobile\files next to the build)
//   --screen WxH      the phone's screen for "fill the screen" (default 3168x1440, Jarrod's Find X8 Ultra)
#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>
#include <thread>

#include "mobile_platform.h"

namespace mlauncher::platform {

namespace {
fs::path g_files;
int g_screen_w = 3168, g_screen_h = 1440;

void Message(std::string text) {
  Event e;
  e.kind = Event::kMessage, e.a = std::move(text);
  Post(std::move(e));
}
}  // namespace

void Init(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--files") g_files = fs::u8path(argv[++i]);
    else if (a == "--screen") std::sscanf(argv[++i], "%dx%d", &g_screen_w, &g_screen_h);
  }
  if (g_files.empty()) g_files = fs::u8path(SDL_GetBasePath()) / "files";
  std::error_code ec;
  fs::create_directories(g_files, ec);
}

fs::path FilesDir() { return g_files; }
const char* Name() { return "desktop"; }

void PickGameFile() {
  static const SDL_DialogFileFilter filters[] = {{"Xbox 360 disc image", "iso"}, {"Game executable", "xex"}};
  SDL_ShowOpenFileDialog(
      [](void*, const char* const* list, int) {
        Event e;
        e.kind = Event::kGamePicked;
        e.ok = list && list[0];
        if (e.ok) {
          fs::path p = fs::u8path(list[0]);
          if (p.extension() == ".xex") p = p.parent_path();  // an extracted folder: its default.xex
          e.a = U8(p), e.b = U8(p.filename());
        }
        Post(std::move(e));
      },
      nullptr, nullptr, filters, 2, nullptr, false);
}

std::string OpenGameFile(const std::string& address) {
  std::error_code ec;
  return fs::exists(fs::u8path(address), ec) ? address : std::string();
}

void AddDlc(const fs::path& dlc_dir) {
  static fs::path dir;
  dir = dlc_dir;
  SDL_ShowOpenFileDialog(
      [](void*, const char* const* list, int) {
        if (!list || !list[0]) return;
        std::vector<fs::path> files;
        for (int i = 0; list[i]; ++i) files.push_back(fs::u8path(list[i]));
        std::thread([files] {
          std::error_code ec;
          fs::create_directories(dir, ec);
          int copied = 0;
          for (const auto& f : files)
            if (fs::copy_file(f, dir / f.filename(), fs::copy_options::overwrite_existing, ec)) ++copied;
          Event e;
          e.kind = Event::kDlcAdded, e.a = "Copied " + std::to_string(copied) + " of " + std::to_string(files.size()) + " files";
          Post(std::move(e));
        }).detach();
      },
      nullptr, nullptr, nullptr, 0, nullptr, true);
}

void Share(const std::vector<fs::path>& files) {
  std::string text = "PC test build: the phone would share";
  for (const auto& f : files) text += " " + U8(f.filename());
  Message(text);
}

void ExportSaves(const fs::path&) { Message("PC test build: save export works on the phone only"); }
void ImportSaves(const fs::path&) { Message("PC test build: save import works on the phone only"); }

void CheckForUpdate(const std::string& version) {
  std::thread([version] {
    Event e;
    e.kind = Event::kUpdate;
    e.update = launcher::CheckForUpdate(version);
    Post(std::move(e));
  }).detach();
}

void OpenUrl(const std::string& url) { SDL_OpenURL(url.c_str()); }

void StartGame(const std::vector<std::string>& args, const std::string& game_address, const TouchChoice& touch) {
  std::ofstream f(g_files / "last_launch.txt", std::ios::trunc);
  f << "# game: " << game_address << "\n# touch: mode " << touch.mode << " size " << touch.size << " opacity "
    << touch.opacity << " vibrate " << touch.vibrate << "\n";
  for (const auto& a : args) f << a << "\n";
  Message("PC test build: the game's command line is in last_launch.txt");
}

void Quit() {
  SDL_Event e{};
  e.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&e);
}

void ScreenSize(int& w, int& h) { w = g_screen_w, h = g_screen_h; }

}  // namespace mlauncher::platform
