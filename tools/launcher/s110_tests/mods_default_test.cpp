// S110 HELPERSTART review: the mods folder a game started by hand takes from the launcher's settings file when the file names none
// (src\dlc_catalog.h LauncherModsDefault, used by src\dlc.h ApplyLauncherSettings). It must be the launcher's own default
// (src\launcher\launcher_core.h FindPaths): <launcher folder>\mods in a release folder, the game's own default (returned empty) in the
// dev layout (game_data\extracted\default.xex in the launcher's folder or above it). Before the review the game took
// <game data>\..\mods in a release too, a folder the launcher never builds into. std only; folders under the system temp folder.
// Build + run: run_mods_default.py (+ planted faults).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "dlc_catalog.h"

namespace fs = std::filesystem;
using wwe2k14::dlc::LauncherModsDefault;

static int fails = 0, checks = 0;
static void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++fails, std::printf("FAIL %s\n", what);
}

static bool RepoAbove(fs::path d) {
  std::error_code ec;
  for (; !d.empty(); d = d.parent_path()) {
    if (fs::exists(d / "game_data" / "extracted" / "default.xex", ec)) return true;
    if (d == d.parent_path()) break;
  }
  return false;
}

int main() {
  std::error_code ec;
  const fs::path tmp =
      fs::temp_directory_path(ec) / ("mods_default_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::remove_all(tmp, ec);
  if (RepoAbove(tmp)) {
    std::printf("FAIL the temp folder %s lies under a game_data\\extracted\\default.xex: the test cannot run there\n", tmp.string().c_str());
    return 1;
  }
  // a. a release folder (nothing above it): <launcher folder>\mods
  const fs::path rel = tmp / "WWE2K14 v0.4.3";
  fs::create_directories(rel, ec);
  Check(LauncherModsDefault(rel) == rel / "mods", "a release folder: the launcher's <folder>\\mods");
  // b. the dev layout: the exe deep under a repo with game_data\extracted\default.xex: the game's own default (empty)
  const fs::path repo = tmp / "repo";
  fs::create_directories(repo / "game_data" / "extracted", ec);
  std::ofstream(repo / "game_data" / "extracted" / "default.xex") << "x";
  const fs::path dev = repo / "out" / "build" / "win-amd64-release";
  fs::create_directories(dev, ec);
  Check(LauncherModsDefault(dev).empty(), "the dev layout (a repo above the exe): the game's own default");
  // c. the exe folder itself holds game_data\extracted\default.xex (the launcher's walk starts at its own folder)
  Check(LauncherModsDefault(repo).empty(), "game_data\\extracted\\default.xex in the exe folder itself: the game's own default");
  // d. a game_data folder without default.xex is not the dev layout
  const fs::path half = tmp / "half";
  fs::create_directories(half / "game_data" / "extracted", ec);
  Check(LauncherModsDefault(half) == half / "mods", "game_data\\extracted with no default.xex: still a release folder");
  // e. no exe folder known: nothing
  Check(LauncherModsDefault(fs::path()).empty(), "no exe folder: nothing");
  fs::remove_all(tmp, ec);
  std::printf("mods_default_test: %d of %d checks passed%s\n", checks - fails, checks, fails ? "" : " (all)");
  return fails ? 1 : 0;
}
