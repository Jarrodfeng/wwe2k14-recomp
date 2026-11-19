// Unit test of the vintage-look hook's decision (S103 VTG2): src\vintage_look_core.h, the very file the hook includes.
//   vintage_look_test.exe            -> shipped logic; exit 0 = pass
//   vintage_look_test.exe --plant N  -> a deliberately wrong policy (positive control): must exit 1. Plants: ignore_flag (always runs the game's draw), always_skip.
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/vintage_look_core.h"

static std::string g_plant;
struct Ctx { int dummy = 0; };
static int g_calls = 0;
static void Original(Ctx&, uint8_t*) { ++g_calls; }

// The policy under test: the shipped Run, or a planted wrong one.
static bool Policy(Ctx& c, bool on) {
  if (g_plant == "ignore_flag") on = true;
  if (g_plant == "always_skip") on = false;
  return wwe2k14::vintage::Run(c, nullptr, on, Original);
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--plant")) g_plant = argv[i + 1];
  int fail = 0;
  Ctx c;
  g_calls = 0;
  bool ran = Policy(c, false);
  if (ran || g_calls != 0) { std::printf("FAIL: OFF still ran the game's draw (calls %d)\n", g_calls); ++fail; }
  ran = Policy(c, true);
  if (!ran || g_calls != 1) { std::printf("FAIL: ON did not run the game's draw exactly once (calls %d)\n", g_calls); ++fail; }
  g_calls = 0;  // a live toggle: flip every call
  for (int i = 0; i < 6; ++i) Policy(c, (i & 1) != 0);
  if (g_calls != 3) { std::printf("FAIL: live toggle ran %d of 3 expected\n", g_calls); ++fail; }
  std::printf("vintage_look_test: %s\n", fail ? "FAIL" : "PASS");
  return fail ? 1 : 0;
}
