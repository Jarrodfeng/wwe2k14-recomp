// wwe2k14 - S110 GRAINRATE (docs\research\s110\GRAINCAP.md): the era film grain's frame rate under the lever. Pure: no game or SDK includes
// (tools\vintage\vintage_look_test.cpp runs this very file; src\vintage_look.h is the hook that calls it).
//
// The grain is VTGE's flipbook, draw sub_82594808 (this = comp+48). Each call: timer (comp+1964) += [0x8392DC74] (+120, the tick step); when the timer
// reaches the period (+1D0: a u32 read from [this+1920], through fcfid, = 3 from the ctor) it is reset to 0 (+1DC), dropping the overshoot, and the
// frame steps (frame + 1) mod 3. So a frame lasts ceil(period / step) updates. The game's own 30 fps scene (step 2) holds a frame 2 updates = 66.7 ms;
// our lever runs that scene at 60 (step 1): 3 updates = 50 ms. The fix is the lever's compensation (same shape as the flash hold, lever_scene.h
// HoldFactor): for the one call, the period word is swapped for GrainPeriod(...) and put back.
#pragma once

#include <cmath>
#include <cstdint>

namespace wwe2k14::vintage {

// period_game: the game's period word (3). k: HoldFactor (our updates per game update of its own scene). own_word: the game's own frame-rate word
// for this scene (30 under the lever; 60 / 30 / 25 as lever_scene.h HoldFactor names it). step_ours: the live tick step [0x8392DC74] (f32).
// Returns the period the timer must reach so a frame lasts as many of OUR updates as the game's own schedule: ceil(P / step_game) * k * step_ours,
// rounded to the nearest whole number, at least 1. k <= 1 (or an unusable input): the game's own period, byte for byte.
inline uint32_t GrainPeriod(uint32_t period_game, uint32_t k, uint32_t own_word, float step_ours) {
  if (k <= 1 || own_word == 0 || !(step_ours > 0.0f)) return period_game;
  const double step_game = 60.0 / double(own_word);   // 2 at the game's own 30; 1 at 60
  const double updates = std::ceil(double(period_game) / step_game - 1e-9);
  const double p = std::floor(updates * double(k) * double(step_ours) + 0.5);
  return p < 1.0 ? 1u : uint32_t(p);
}

// The period word is only swapped when it is sane (the ctor sets 3): 0 or above 64 = not swapped (the caller counts it).
inline bool GrainPeriodSane(uint32_t period_game) { return period_game != 0 && period_game <= 64; }

}  // namespace wwe2k14::vintage
