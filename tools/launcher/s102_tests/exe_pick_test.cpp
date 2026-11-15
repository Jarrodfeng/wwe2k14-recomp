// Session 102 lane W1: which game exe a CPU gets (src\launcher\exe_pick.h), no CPU, no files, no game.
// Build + run: clang++ -std=c++20 -I src/launcher tools/launcher/s102_tests/exe_pick_test.cpp -o out/exe_pick_test.exe && out/exe_pick_test.exe
// Planted faults (each must FAIL this test): run_exe_pick.py builds it against copies of exe_pick.h with one rule removed (the AVX2 bit, the F16C bit,
// the version match, the explicit-v3 fallback, the --wwe2k14_build override).
#include <cstdio>
#include <string>

#include "exe_pick.h"

using namespace launcher;

static int fails = 0, checks = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    ++checks;                                          \
    if (!(c)) {                                        \
      ++fails;                                         \
      std::printf("FAIL %s: ", #c);                    \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static CpuBits Haswell() {
  CpuBits c;
  c.fma = c.movbe = c.popcnt = c.avx = c.os_ymm = c.bmi1 = c.avx2 = c.bmi2 = c.lzcnt = c.f16c = true;
  return c;
}

int main() {
  const CpuBits full = Haswell();
  CHECK(V3Capable(full), "a Haswell-class CPU runs v3");
  CHECK(!V3Capable(CpuBits{}), "an empty CPU does not");

  // Every single missing bit gives the older build (avx2 and f16c are the ones the planted faults drop).
  const char* names[10] = {"fma", "movbe", "popcnt", "avx", "os_ymm", "bmi1", "avx2", "bmi2", "lzcnt", "f16c"};
  for (int drop = 0; drop < 10; ++drop) {
    CpuBits c = full;
    bool* f[10] = {&c.fma, &c.movbe, &c.popcnt, &c.avx, &c.os_ymm, &c.bmi1, &c.avx2, &c.bmi2, &c.lzcnt, &c.f16c};
    *f[drop] = false;
    CHECK(!V3Capable(c), "CPU without %s must not be judged v3", names[drop]);
    for (BuildChoice ch : {BuildChoice::kAuto, BuildChoice::kAvx2, BuildChoice::kBase})
      CHECK(!UseV3(ch, true, true, c), "CPU without %s gets the older build, choice %d", names[drop], int(ch));
    CHECK(GameExeFor("", "C:\\g", true, true, "", c).path == "C:\\g\\wwe2k14.exe", "automatic pick without %s", names[drop]);
    CHECK(GameExeFor("", "C:\\g", true, true, "--wwe2k14_build=avx2", c).path == "C:\\g\\wwe2k14.exe", "avx2 asked, CPU without %s", names[drop]);
    // Review E: Settings names the AVX2 exe explicitly on this CPU: the sibling base exe + a status line.
    const ExePick e = GameExeFor("D:\\my games\\wwe2k14_v3.exe", "C:\\g", true, true, "", c);
    CHECK(e.path == "D:\\my games\\wwe2k14.exe" && !e.note.empty(), "explicit v3 path, CPU without %s -> '%s' note '%s'", names[drop], e.path.c_str(), e.note.c_str());
  }

  // Review F: the v3 exe must be the same build as the base exe.
  CHECK(!UseV3(BuildChoice::kAuto, true, false, full), "version mismatch -> older build");
  CHECK(!UseV3(BuildChoice::kAvx2, true, false, full), "version mismatch, avx2 asked -> older build");
  CHECK(GameExeFor("", "C:\\g", true, false, "", full).path == "C:\\g\\wwe2k14.exe", "mismatch through GameExeFor");

  // The package: no v3 exe in the folder -> older build, whatever the CPU.
  CHECK(!UseV3(BuildChoice::kAuto, false, true, full), "no v3 file");
  CHECK(!UseV3(BuildChoice::kAvx2, false, true, full), "no v3 file, avx2 asked");
  // The normal pick, and the overrides.
  CHECK(UseV3(BuildChoice::kAuto, true, true, full), "auto on a capable CPU picks v3");
  CHECK(UseV3(BuildChoice::kAvx2, true, true, full), "avx2 on a capable CPU picks v3");
  CHECK(!UseV3(BuildChoice::kBase, true, true, full), "base always picks the older build");

  // Review D: the Extra flags decide the exe at once, with no reload step in between (pure function of the flags).
  CHECK(GameExeFor("", "C:\\g", true, true, "", full).path == "C:\\g\\wwe2k14_v3.exe", "no flag -> v3");
  CHECK(GameExeFor("", "C:\\g", true, true, "--a=1 --wwe2k14_build=base", full).path == "C:\\g\\wwe2k14.exe", "flag base -> base, same call");
  CHECK(GameExeFor("", "C:\\g", true, true, "--wwe2k14_build=base --wwe2k14_build=avx2", full).path == "C:\\g\\wwe2k14_v3.exe", "last flag wins");
  // An explicit non-v3 path is left alone; an explicit v3 path on a capable CPU too.
  CHECK(GameExeFor("D:\\x\\game.exe", "C:\\g", true, true, "", full).path == "D:\\x\\game.exe", "explicit path kept");
  CHECK(GameExeFor("D:\\x\\WWE2K14_V3.EXE", "C:\\g", true, true, "", full).path == "D:\\x\\WWE2K14_V3.EXE", "explicit v3 on a capable CPU kept");
  CHECK(GameExeFor("wwe2k14_v3.exe", "C:\\g", true, true, "", CpuBits{}).path == "wwe2k14.exe", "bare v3 name, no AVX2");

  // The flag.
  CHECK(ParseBuildChoice("") == BuildChoice::kAuto, "none");
  CHECK(ParseBuildChoice("--wwe2k14_build=avx2") == BuildChoice::kAvx2, "avx2");
  CHECK(ParseBuildChoice("--x=1 --wwe2k14_build=base --y") == BuildChoice::kBase, "base in the middle");
  CHECK(ParseBuildChoice("--wwe2k14_build=base --wwe2k14_build=avx2") == BuildChoice::kAvx2, "the last one wins");
  CHECK(ParseBuildChoice("--wwe2k14_build=nonsense") == BuildChoice::kAuto, "nonsense is auto");
  CHECK(ParseBuildChoice("--foo--wwe2k14_build=base") == BuildChoice::kAuto, "must start a word");
  CHECK(StripBuildChoice("--x=1 --wwe2k14_build=base --y") == "--x=1 --y", "strip middle: '%s'", StripBuildChoice("--x=1 --wwe2k14_build=base --y").c_str());
  CHECK(StripBuildChoice("--wwe2k14_build=avx2") == "", "strip only");
  CHECK(StripBuildChoice("--a --wwe2k14_build=avx2") == "--a", "strip last");
  CHECK(StripBuildChoice("--a --b") == "--a --b", "strip none");

  std::printf("exe_pick_test: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
