// Unit test of the vintage-look hook's decision (S103 VTG2): src\vintage_look_core.h, the very file the hook includes.
//   vintage_look_test.exe            -> shipped logic; exit 0 = pass
//   vintage_look_test.exe --plant N  -> a deliberately wrong policy (positive control): must exit 1. Plants: ignore_flag (always runs the game's draw), always_skip.
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/vintage_grain_rate_core.h"
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

// S110 GRAINRATE: the shipped GrainPeriod, or the planted "no_swap" (the game's period 3 kept at k 2: the 50 ms schedule)
static uint32_t GrainPeriodUnderTest(uint32_t p, uint32_t k, uint32_t own, float step) {
  if (g_plant == "no_swap") return p;
  return wwe2k14::vintage::GrainPeriod(p, k, own, step);
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
  // S110 GRAINRATE (src\vintage_grain_rate_core.h): the table
  struct Row { uint32_t p, k, own; float step; uint32_t want; };
  const Row rows[] = {{3, 1, 30, 1.0f, 3}, {3, 2, 30, 1.0f, 4}, {3, 4, 30, 0.5f, 4}, {3, 1, 60, 1.0f, 3}, {3, 2, 60, 0.5f, 3}, {3, 2, 25, 1.0f, 4}};
  for (const Row& r : rows) {
    const uint32_t got = GrainPeriodUnderTest(r.p, r.k, r.own, r.step);
    if (got != r.want) { std::printf("FAIL: GrainPeriod(%u, %u, %u, %g) = %u, want %u\n", r.p, r.k, r.own, double(r.step), got, r.want); ++fail; }
  }
  if (wwe2k14::vintage::GrainPeriodSane(0) || wwe2k14::vintage::GrainPeriodSane(65) || !wwe2k14::vintage::GrainPeriodSane(3)) { std::printf("FAIL: GrainPeriodSane\n"); ++fail; }
  // the simulated timer (sub_82594808: timer += step; timer >= period -> timer = 0, frame++): updates per grain picture
  struct Sim { uint32_t k, own; float step; uint32_t want_updates; };
  const Sim sims[] = {{1, 30, 1.0f, 3}, {2, 30, 1.0f, 4}, {4, 30, 0.5f, 8}, {1, 30, 2.0f, 2}};   // the last: the game's own 30 fps scene (66.7 ms)
  for (const Sim& s : sims) {
    const float period = float(GrainPeriodUnderTest(3, s.k, s.own, s.step));
    float timer = 0; uint32_t calls = 0, frames = 0, first = 0;
    for (int i = 1; i <= 240; ++i) { timer += s.step; if (timer >= period) { timer = 0; if (!first) first = i; ++frames; } ++calls; }
    // the frame must step every want_updates updates at the sim's own step; the time per picture is that count x the update time (checked by the first step)
    const uint32_t per = frames ? calls / frames : 0;
    if (first != s.want_updates || per != s.want_updates) { std::printf("FAIL: k %u step %g: a grain picture lasts %u updates (first %u), want %u\n", s.k, double(s.step), per, first, s.want_updates); ++fail; }
  }
  std::printf("vintage_look_test: %s\n", fail ? "FAIL" : "PASS");
  return fail ? 1 : 0;
}
