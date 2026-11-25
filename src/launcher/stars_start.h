// S110 HELPERSTART: before every start of the game that loads the save (PLAY, the logo import), the one decision "is the imported
// superstars' pack up to date for this start? if not, build it first". Session 101 (lane 199) made it for PLAY only (StarsBeforeStart); the helper
// runs then offered the pack as it was last built (S110 LOGOIMPORT review F1). The game leaves pack 6 out when it is an old pack6\ with no frame,
// not built at all, or a frame that no longer fits the offered DLC (src\superstars.h Load, src\dlc.h Install): exactly when importer::CheckBeforePlay
// says "build". A save whose DLC mask has bit 6 then gets the game's missing-content notice, which a helper's route cannot answer.
// Header-only, std only: tools\launcher\s110_tests\stars_start_test.cpp compiles and checks it (run_stars_start.py: + planted faults).
// launcher_main.cpp StarsBeforeStart fills Ask, runs the check through Decide, and starts the build / says the wait.
// Build previews does NOT take it (review F-3): the tron export runs from the VdSwap hook at guest frame 900 whatever screen is up
// (src\tron_export.h OnSwap), presses nothing and quits; the save's notice comes only after START. The pack cannot change what it makes.
#pragma once
#include <string>

namespace launcher::stars_start {

// Which start asked: what runs again after the build, and what the player is told.
enum class Then { kPlay, kLogoImport };
// kStart: start now. kWait: a build is running already: this start does not start (it says so). kBuild: build first, then the same start again.
enum class Step { kStart, kWait, kBuild };

struct Ask {
  bool build_running = false;   // a superstars build is running (the launcher's stars_job_)
  bool game_exe = false;        // the command starts the game exe (not pwsh / a harness route)
  bool stars_flag = false;      // the command offers the imported superstars' pack (HasStarsFlag)
  bool built_for_this = false;  // the last build was made for exactly this command (stars_ok_for_)
  bool test_run = false;        // harness, performance test, benchmark, determinism check, diagnostic run, a logo import's game running
};

// The command text carries the switch (the same test StarsBeforeStart made since Session 101: a find in the shown command).
inline bool HasStarsFlag(const std::string& display) { return display.find("--wwe2k14_mods_superstars=true") != std::string::npos; }

// `check` reads the pack (importer::CheckBeforePlay, then LoadPack): true = a build is needed and can start. It is run only when the
// answer depends on it, so a start without the switch reads nothing from disk (as before).
template <class Check>
Step Decide(const Ask& a, Check&& check) {
  if (a.build_running) return Step::kWait;  // (PLAY's Session 101 order: this came first, for every command)
  if (!a.game_exe || !a.stars_flag || a.built_for_this || a.test_run) return Step::kStart;
  return check() ? Step::kBuild : Step::kStart;
}

// Only PLAY offers PLAY WITHOUT THEM after a failed build: a helper run without the pack meets the save's notice (it loads the save).
inline bool OffersWithout(Then t) { return t == Then::kPlay; }

// Said (status line; the logo page's result too) when a start meets a build that is already running.
inline const char* WaitText(Then t) {
  switch (t) {
    case Then::kLogoImport: return "Your superstars are being built into the game: import your logos when that is done";
    default: return "Your superstars are being built into the game: the game starts when that is done";
  }
}
// The end of YOUR SUPERSTARS' building sentence ("... from your own files <this>...").
inline const char* BeforeText(Then t) {
  switch (t) {
    case Then::kLogoImport: return "before the logo import starts";
    default: return "before it starts";
  }
}
// The second sentence after a failed build (the first says why).
inline const char* FailText(Then t) {
  switch (t) {
    case Then::kLogoImport:
      return "The logo import did not start: it loads your save, which may need them. Open the Superstar Importer (MODS > SUPERSTARS) to fix "
             "it, then import again.";
    default: return "Open the Superstar Importer (MODS > SUPERSTARS) to fix it, or play without them this time.";
  }
}

}  // namespace launcher::stars_start
