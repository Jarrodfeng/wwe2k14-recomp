// S102 lane C5 (docs\renderer\COPIES_LEFT.md section 6): the rule of the direct switch (wwe2k14_rdr_copy_direct) as a PURE function, so a unit test (tools\c5\copy_direct_test.cpp) plants every rule away.
// No game, SDK or renderer includes. The binder (binder.h DirectCheck) gathers the Facts from the frame's own ops and calls Decide; copy_alias.h's planner counts the reasons.
//
// A copy S -> D the strict alias rule admits is REMOVED when its pass can render straight into D. What the pass must look like, and what the removal leaves behind (review F1/F2/F4):
//   - the pass: exactly one SetTargets of the frame has S as a colour target, it has ONE colour target, nothing but clears and draws sits between it and the copy, S is copied exactly once;
//   - S is no longer written, so nobody may read it or write it elsewhere in the frame: never sampled (no Sampled usage), no copy INTO S, no Present of S;
//   - the first op of the pass is a FULL colour clear, so the pixels the draw does not cover are the clear colour in D exactly as they were in S (not the last frame's leftovers);
//   - the copy is the whole picture 1:1 (both rects full and equal, equal size / format / kind, mip and layer 0);
//   - D was made with target usage, was never read back by the CPU, and S and D follow the render scale together or not at all;
//   - S has not been banned by the stale-read ledger (a later frame read S before drawing it: the removal left that read stale).
#pragma once
#include <cstdint>

namespace rdr_adapter::copy_direct {

// Why a copy that the strict rule admitted was kept (kDirectOk = taken). Appended only.
enum DirectWhy : int {
  kDirectOk = 0, kDirectNoPass, kDirectSourceShared, kDirectNotTarget, kDirectClass, kDirectManyTargets, kDirectScaled, kDirectBakeSkip, kDirectNoClear, kDirectFault, kDirectStale, kDirectOtherPlanner, kDirectRect,
  kDirectWhyCount
};
inline const char* DirectWhyName(int w) {
  static const char* const k[] = {"taken", "no single pass of the source before the copy", "the source is read or written elsewhere", "destination not made target-capable", "class / size not taken",
                                  "pass has several colour targets", "one of the pair follows the render scale", "crowd bake skip", "the pass does not start with a full colour clear", "a planted alias fault is on",
                                  "the source was read stale in a later frame (banned)", "another planner took the copy", "the copy is not a whole 1:1 picture"};
  static_assert(sizeof(k) / sizeof(k[0]) == size_t(kDirectWhyCount), "one name per reason");
  return w >= 0 && w < kDirectWhyCount ? k[w] : "?";
}

struct Facts {
  uint8_t sets = 0;            // SetTargets of the frame with S as a colour target
  uint8_t copies = 0;          // copies of the frame with S as their source
  uint8_t copy_dests = 0;      // copies of the frame INTO S
  uint8_t presents = 0;        // Presents of S
  bool source_sampled = false;       // S has Sampled usage (a draw could read it)
  bool dest_read_back = false;       // D was ever read back by the CPU
  bool dest_target_capable = false;  // D was made with target usage (DirectShape at image creation)
  bool pass_before_copy = false;     // the one SetTargets of S sits before the copy
  uint8_t pass_colour_targets = 1;   // colour targets of that SetTargets
  bool only_clears_draws = false;    // between that SetTargets and the copy: clears and draws only
  bool first_op_full_clear = false;  // the op after that SetTargets is a Clear with kClearColor, full, colour target 0
  bool whole_copy = false;           // both rects start at 0,0 and cover the whole picture, equal; same size / format / kind; mip 0, layer 0
  bool scale_mismatch = false;       // exactly one of S / D has the Scaled usage
  bool banned = false;               // the stale-read ledger banned S
};

// Planted weakenings (the unit test): each skips one rule and must make a case of the test fail.
enum class Plant : uint8_t { None, NoClear, NoCopyInto, NoPresent, NoWhole, NoLedger, NoShared, NoScale, Count };

inline int Decide(const Facts& f, Plant plant = Plant::None) {
  if (plant != Plant::NoScale && f.scale_mismatch) return kDirectScaled;
  if (!f.dest_target_capable) return kDirectNotTarget;
  if (plant != Plant::NoLedger && f.banned) return kDirectStale;
  if (plant != Plant::NoWhole && !f.whole_copy) return kDirectRect;
  if (plant != Plant::NoShared && (f.sets != 1 || f.copies != 1 || f.source_sampled || f.dest_read_back)) return kDirectSourceShared;
  if (plant != Plant::NoCopyInto && f.copy_dests) return kDirectSourceShared;
  if (plant != Plant::NoPresent && f.presents) return kDirectSourceShared;
  if (!f.pass_before_copy) return kDirectNoPass;
  if (f.pass_colour_targets != 1) return kDirectManyTargets;
  if (!f.only_clears_draws) return kDirectNoPass;
  if (plant != Plant::NoClear && !f.first_op_full_clear) return kDirectNoClear;
  return kDirectOk;
}

}  // namespace rdr_adapter::copy_direct
