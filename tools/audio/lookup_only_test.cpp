// Lane 283: the GAME's view of the song cache. Links src\audio_import\lookup.cpp ONLY (no decoders, no Import: the game executable
// contains nothing else of the importer). Run on the folder import_selftest.exe prepared (LOOKUP_CASE): a mods folder with a CJK name,
// holding a converted song, a refused song and a song nobody converted. Exit 0 = every expectation held.
#include <cstdio>
#include <filesystem>

#include "../../src/audio_import/audio_import.h"

namespace fs = std::filesystem;
using namespace audio_import;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  if (!ok) ++g_fail;
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::printf("== lookup-only binary (the game's view)\n");
  try {
    const fs::path mods = fs::path(argv[1]) / fs::path(u8"モッズ★唱");
    const fs::path dir = mods / "music" / "new", cache = mods / "music" / ".cache";
    Imported ia, ib, ic;
    const fs::path pa = PlayableNow(dir / fs::path(u8"唱★converted.wav"), cache, kTargetLufs, &ia);
    const fs::path pb = PlayableNow(dir / fs::path(u8"壊れた.mp3"), cache, kTargetLufs, &ib);
    const fs::path pc = PlayableNow(dir / fs::path(u8"notyet★.wav"), cache, kTargetLufs, &ic);
    Check(ia.ok && ia.normalised && pa != dir / fs::path(u8"唱★converted.wav") && fs::is_regular_file(pa), "the converted song plays from the cache");
    Check(pb.empty() && !ib.ok && !ib.err.empty(), "the refused song gives an empty path and a reason");
    Check(pc == dir / fs::path(u8"notyet★.wav") && !ic.ok && ic.err.empty(), "the unconverted song plays as it is ('not converted yet')");
    Check(Stats().imports.load() == 0 && Stats().decodes.load() == 0, "no Import call and no decode in this process");
    Check(PlayableNow(dir / "no_such_file.wav", cache).empty(), "a missing file gives an empty path");
    // the index alone (this process has no memo): the key is folded, so another letter case of the same file is found
    Imported iu;
    const fs::path pu = PlayableNow(dir / fs::path(u8"唱★CONVERTED.WAV"), cache, kTargetLufs, &iu);
    Check(iu.ok && iu.normalised && pu == pa, "the same file under another letter case is found through the index (relative, case-folded key)");
  } catch (...) {
    Check(false, "nothing throws (CJK mods folder + CJK song names)");
  }
  std::printf("%s\n", g_fail ? "FAILED" : "all ok");
  return g_fail ? 1 : 0;
}
