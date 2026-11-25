// S110 HELPERSTART: the one "superstars pack before a start" decision (src\launcher\stars_start.h), no ImGui, no files, no game.
// Build + run: clang++ -std=c++20 -I src/launcher tools/launcher/s110_tests/stars_start_test.cpp -o out/stars_start_test.exe && out/stars_start_test.exe
// Planted faults (each must FAIL this test): run_stars_start.py builds it against copies of stars_start.h with one rule changed.
#include <cstdio>
#include <cstring>
#include <string>

#include "stars_start.h"

using namespace launcher::stars_start;

static int fails = 0, checks = 0;
#define CHECK(c, ...)               \
  do {                              \
    ++checks;                       \
    if (!(c)) {                     \
      ++fails;                      \
      std::printf("FAIL %s: ", #c); \
      std::printf(__VA_ARGS__);     \
      std::printf("\n");            \
    }                               \
  } while (0)

// A start of the game with the switch on, nothing in the way: the check decides.
static Ask Plain() {
  Ask a;
  a.game_exe = a.stars_flag = true;
  return a;
}

// Decide with a check that answers `need` and counts its calls.
static Step Run(const Ask& a, bool need, int* calls) {
  return Decide(a, [&] {
    ++*calls;
    return need;
  });
}

int main() {
  int n = 0;
  // 1. the check decides when nothing else does
  CHECK(Run(Plain(), true, &n) == Step::kBuild && n == 1, "a pack that needs a build is built first (check ran %d time(s))", n);
  n = 0;
  CHECK(Run(Plain(), false, &n) == Step::kStart && n == 1, "a pack that is up to date: start now");

  // 2. a build already running: wait, for EVERY command (PLAY's Session 101 order), and the check is not run
  {
    Ask a;
    a.build_running = true;
    n = 0;
    CHECK(Run(a, true, &n) == Step::kWait && n == 0, "no switch, a build running: the start waits (PLAY's old first test)");
    Ask b = Plain();
    b.build_running = b.test_run = b.built_for_this = true;
    n = 0;
    CHECK(Run(b, true, &n) == Step::kWait && n == 0, "a build running wins over every other answer");
  }

  // 3. each reason to start without a build, alone; none of them reads the pack
  const char* names[4] = {"not the game exe", "no superstars switch", "built for this command", "a test run"};
  for (int k = 0; k < 4; ++k) {
    Ask a = Plain();
    if (k == 0) a.game_exe = false;
    if (k == 1) a.stars_flag = false;
    if (k == 2) a.built_for_this = true;
    if (k == 3) a.test_run = true;
    n = 0;
    CHECK(Run(a, true, &n) == Step::kStart && n == 0, "%s: start now, the pack is not read", names[k]);
  }

  // 4. the switch test is the one StarsBeforeStart made (a find in the shown command)
  CHECK(HasStarsFlag("\"g.exe\" --wwe2k14_dlc=true --wwe2k14_mods_superstars=true --user_language=1"), "the flag in the middle");
  CHECK(HasStarsFlag("--wwe2k14_mods_superstars=true"), "the flag alone");
  CHECK(!HasStarsFlag("\"g.exe\" --wwe2k14_dlc=true"), "no flag");
  CHECK(!HasStarsFlag("--wwe2k14_mods_superstars=false"), "the flag off");

  // 5. PLAY WITHOUT THEM only for PLAY: a helper run without the pack meets the save's notice
  CHECK(OffersWithout(Then::kPlay), "PLAY offers PLAY WITHOUT THEM");
  CHECK(!OffersWithout(Then::kLogoImport), "the logo import does not");

  // 6. PLAY's words are the Session 101 words, unchanged; each helper has its own
  CHECK(!std::strcmp(WaitText(Then::kPlay), "Your superstars are being built into the game: the game starts when that is done"), "PLAY's wait line");
  CHECK(!std::strcmp(BeforeText(Then::kPlay), "before it starts"), "PLAY's building sentence");
  CHECK(!std::strcmp(FailText(Then::kPlay), "Open the Superstar Importer (MODS > SUPERSTARS) to fix it, or play without them this time."),
        "PLAY's failure sentence");
  for (Then t : {Then::kLogoImport}) {
    CHECK(std::strcmp(WaitText(t), WaitText(Then::kPlay)) && std::strcmp(BeforeText(t), BeforeText(Then::kPlay)) &&
              std::strcmp(FailText(t), FailText(Then::kPlay)),
          "helper %d has its own words", int(t));
    CHECK(!std::strstr(FailText(t), "play without them"), "helper %d never offers playing without them", int(t));
  }

  std::printf("stars_start_test: %d of %d checks passed%s\n", checks - fails, checks, fails ? "" : " (all)");
  return fails ? 1 : 0;
}
