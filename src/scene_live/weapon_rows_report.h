// S108 AL1 (docs\research\s108\AL1_plan.md row 3): a weapon-row call refused over the bound, said through the shared report (rdr::DrawShortfall, kind `row_bound`). Copyright (c) the wwe2k14-recomp
// authors. BSD-3-Clause. Header only, no game or SDK includes: the glue (weapon_rows_glue.h RawEntry, on the guest thread that made the call) and the offline unit (tools\weapons\al1_sites_unit.cpp) call THIS.
// Was FAN's rdr::Problem("weapon_rows_over_bound") + a REXLOG line + its own mutex / set: the shared report keeps the once-per-model key itself (and never aborts a dev build).
#pragma once
#include <cstdint>
#include <string>

#include "../renderer/problem.h"
#include "weapon_rows_core.h"   // CallResult, kRefusedOverBound, kWeaponMaxNodes

namespace rdr_scene::live::weaponrows {

// The name a person needs to find the model of a call (the drawer's ItemName uses the same words: pose_rows_report.h).
inline std::string CallName(const CallResult& r) {
  return r.event ? "event prop " + std::to_string(r.id) + " entry " + std::to_string(r.k) : r.stage ? "stage PACH " + std::to_string(r.id) + " k " + std::to_string(r.k) : "weapon id " + std::to_string(r.id);
}

// true = the call was refused over the bound and reported (once per (model name) a run). The pool-full refusal is not judged here (the frame's pool: lane AL2's).
inline bool ReportRefused(const CallResult& r, uint32_t model) {
  if (r.refused != kRefusedOverBound) return false;
  rdr::DrawShortfall("row_bound", CallName(r).c_str(), uint64_t(r.n < 0 ? 0 : r.n), 0, "model 0x%08X: over the bound %u: refused, not drawn", model, kWeaponMaxNodes);
  return true;
}

}  // namespace rdr_scene::live::weaponrows
