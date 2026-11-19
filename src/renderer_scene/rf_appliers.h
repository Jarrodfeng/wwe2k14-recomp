// The class appliers of record_frame.h (Session 103, lane R). Each is in its own small file (rf_arena.cpp, rf_crowd.cpp, rf_models.cpp) so the class lanes
// (B wrestlers, C arena / ring / props / screens, D crowd) replace one file and one row of the table in record_frame_classes.cpp, nothing else.
// An applier appends its objects to ctx.frame (through ctx.Emit: the space rule) and its ClassReports to ctx.reports; it skips (and says why) what its family mask
// leaves out. It runs on the render thread every live frame: ask the cache for assets (never the AssetSource), build nothing it already built, print nothing twice.
#pragma once
#include "record_frame.h"

namespace rdr_scene::rf {

bool ApplyArena(BuildCtx& ctx);    // rf_arena.cpp: arena slot objects -> one arena model, its CPU state (light sets, slots shown) updated from the record
bool ApplyCrowd(BuildCtx& ctx);    // rf_crowd.cpp: crowd bodies posed per palette into bounded pools (assets::crowd_body) + cards
bool ApplyModels(BuildCtx& ctx);   // rf_models.cpp: every other model object (wrestlers, ring, props ...) by its asset name

// Helpers shared by the appliers.
inline bool WantFamily(const BuildCtx& c, Family f) { return (c.families & (1u << uint32_t(f))) != 0; }
// The report of `f` in ctx.reports (made on first use).
ClassReport& ReportFor(BuildCtx& ctx, Family f);
// A `why` from ModelCache::Get / Stored that leaves a HOLE (everything but "not resident", which is a counted miss).
bool IsHole(const std::string& why);
// One skip: counts it, remembers the first reason, counts a hole when the reason is one.
inline void Skip(ClassReport& rep, const std::string& why, uint32_t n = 1) {
  rep.skipped += n;
  if (IsHole(why)) rep.holes += n;
  if (rep.why.empty()) rep.why = why;
}

}  // namespace rdr_scene::rf
