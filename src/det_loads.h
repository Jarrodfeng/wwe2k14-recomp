// wwe2k14 - S110 DETLOADS: the load speed-ups (load turbo, the fast loader) under deterministic mode (docs/research/s110/REPMODE.md).
// Under det mode (the game's clocks virtual, its threads taking turns) the speed-ups read the wall clock in exactly three places:
//   1. the load turbo's wall cap       (load_turbo.h, window end by host time)
//   2. its 2D animation throttle        (load_turbo.h, sub_82BBEC18 skipped by host time)
//   3. the fast loader's time budget    (load_probe.h, passes cut by host time)
// With --wwe2k14_replay_loads=fast and det on, each of the three is replaced by a guest-time rule, so a recording keeps the
// speed-ups and still repeats. Anything else (det off, or loads slow = today) keeps every rule as it was.
// The predicates are plain functions of two bools (unit: tools/report/det_loads_unit.cpp, which defines DET_LOADS_PURE and needs no SDK);
// the argument-free forms read the real state.
#pragma once

#include <cstdint>

namespace wwe2k14::det_loads {

// det on AND fast replay loads chosen = the wall-clock rule is replaced. Every other combination keeps the wall-clock rule.
inline constexpr bool GuestRules(bool det_on, bool loads_fast) { return det_on && loads_fast; }
inline constexpr bool WallCapApplies(bool det_on, bool loads_fast) { return !GuestRules(det_on, loads_fast); }
inline constexpr bool UiThrottleApplies(bool det_on, bool loads_fast) { return !GuestRules(det_on, loads_fast); }
inline constexpr bool FastLoaderBudgetApplies(bool det_on, bool loads_fast) { return !GuestRules(det_on, loads_fast); }

// The cap in GUEST frames that replaces the wall cap under det. DERIVED, not measured: the cap in ms (default 8000) at the game's
// 60 Hz = 8000 * 60 / 1000 = 480 frames (rounded up; the det vblank is the 60 Hz frame, REPMODE.md R1).
inline constexpr uint64_t GuestFrameCap(int64_t cap_ms, int hz = 60) {
  return cap_ms <= 0 ? 0 : uint64_t((cap_ms * hz + 999) / 1000);
}

}  // namespace wwe2k14::det_loads

#ifndef DET_LOADS_PURE
#include <rex/cvar.h>
#include <rex/det.h>

#include <string>

REXCVAR_DECLARE(std::string, wwe2k14_replay_loads);

namespace wwe2k14::det_loads {
inline bool LoadsFast() { return REXCVAR_GET(wwe2k14_replay_loads) == "fast"; }
inline bool WallCapApplies() { return WallCapApplies(rex::det::Enabled(), LoadsFast()); }
inline bool UiThrottleApplies() { return UiThrottleApplies(rex::det::Enabled(), LoadsFast()); }
inline bool FastLoaderBudgetApplies() { return FastLoaderBudgetApplies(rex::det::Enabled(), LoadsFast()); }
}  // namespace wwe2k14::det_loads
#endif
