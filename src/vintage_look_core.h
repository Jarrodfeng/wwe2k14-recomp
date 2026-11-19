// The vintage-look switch, decision only (S103 VTG2; docs\research\s103\VTG.md). The stage component "VTGE" (ctor sub_825942E8) draws the retro arenas' darkening rect, grain flipbook
// and vignette corners in its draw method sub_82594808 (this = component + 48). One player setting (PlayerQuality::film_grain) covers them and the sepia: when it is off the method is not
// run, so no 2D op is recorded for those draws (both builds lose exactly them). The method's render-state save / restore is symmetric, so skipping it whole leaves the thread state alone.
// No game or SDK includes: tools\vintage\vintage_look_test.cpp runs this very file; src\vintage_look.h is the hook that calls it.
#pragma once

#include <cstdint>

namespace wwe2k14::vintage {

// Runs `original` only while the vintage look is on. Returns true when the game's draw ran.
template <class Ctx, class Original>
inline bool Run(Ctx& ctx, uint8_t* base, bool vintage_on, Original original) {
  if (!vintage_on) return false;
  original(ctx, base);
  return true;
}

}  // namespace wwe2k14::vintage
