// wwe2k14 - lane 303: the launcher's conversion, as a command line, for the combined custom-music live check.
//   music_live_import.exe MODS_DIR
// For every supported audio file directly in <MODS_DIR>\music\, <MODS_DIR>\music\new\ and <MODS_DIR>\music\library\ it calls the REAL
// importer (audio_import::Import, the call the launcher's Add a song / convert pass makes), which writes the normalised copy and
// <MODS_DIR>\music\.cache\index.txt through the same code. Then it asks the GAME's lookup (audio_import::PlayableNow, what lookup.cpp
// gives the game process) for each file and prints what the game will play. Nothing is hand-written into the cache.
// Output (tab separated, one per file):
//   FILE <path> <status ok|plain|refused> <game_plays> <out_lufs> <true_peak_db> <seconds> [note]
// then DONE <n> file(s) <m> refused. Exit code 0 when every file is ok/plain, 1 when one was refused, 2 on a usage error.
// Build: tools\audio\music_live_import_build.ps1. MODS_DIR must be spelled the way the game gets it (--wwe2k14_mods_dir): the index key
// is the path.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/audio_import/audio_import.h"

namespace fs = std::filesystem;

static std::string U8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: music_live_import MODS_DIR\n");
    return 2;
  }
  const fs::path mods = fs::u8path(argv[1]);
  const fs::path cache = mods / "music" / ".cache";
  std::vector<fs::path> files;
  for (const char* sub : {"", "new", "library"}) {
    const fs::path dir = sub[0] ? mods / "music" / sub : mods / "music";
    std::error_code ec;
    std::vector<fs::path> here;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (it->is_regular_file(e2) && audio_import::IsSupportedExt(it->path())) here.push_back(it->path());
    }
    std::sort(here.begin(), here.end());
    files.insert(files.end(), here.begin(), here.end());
  }
  int bad = 0;
  for (const auto& f : files) {
    const audio_import::Imported r = audio_import::Import(f, cache, audio_import::kTargetLufs);
    audio_import::Imported view;
    const fs::path plays = audio_import::PlayableNow(f, cache, audio_import::kTargetLufs, &view);
    const char* status = !r.ok ? "refused" : r.normalised ? "ok" : "plain";
    if (!r.ok) ++bad;
    std::printf("FILE\t%s\t%s\t%s\t%.2f\t%.2f\t%.2f\t%s\n", U8(f).c_str(), status, U8(plays).c_str(), r.out_lufs,
                r.out_true_peak_db, r.seconds, (r.ok ? r.note : r.err).c_str());
  }
  std::printf("DONE\t%zu file(s)\t%d refused\n", files.size(), bad);
  return bad ? 1 : 0;
}
