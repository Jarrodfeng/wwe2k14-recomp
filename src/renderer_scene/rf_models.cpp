// The generic model applier of record_frame.h (Session 103, lane R; moved out of tools\scene\rsc_draw.cpp as it was): every object that is not an arena slot or a
// crowd body is drawn as the model its asset names (wrestlers by pac\ch + attire, props by pac\evt\evtobj.pac + entry, a viewer record's whole arena). Lane B
// replaces the wrestlers' part, lane C the ring / props / screens parts, in their own files. Assets come from the cache only (a miss is queued, a failed load is
// remembered); a skip that leaves a hole is counted in ClassReport::holes.
#include <cstdio>

#include "rf_appliers.h"

namespace rdr_scene::rf {

bool ApplyModels(BuildCtx& ctx) {
  const SceneTables& t = ctx.t;
  for (size_t i = 0; i < t.objects.size(); ++i) {
    const Object& o = t.objects[i];
    const Family f = ctx.family(i);
    const AssetInfo::Kind kind = ctx.info(o.asset).kind;
    if (kind == AssetInfo::Crowd && f == Family::Crowd) continue;  // ApplyCrowd
    if (kind == AssetInfo::ArenaSlot) continue;                    // ApplyArena: drawn once per arena
    if (f == Family::Wrestlers) continue;                          // S103 S (integration): lane B's row (rf_wrestlers.cpp) draws them
    if (!WantFamily(ctx, f)) continue;
    ClassReport& rep = ReportFor(ctx, f);
    std::string why;
    const int m = ctx.models.Get(t, o.asset, &why);
    if (m < 0) { Skip(rep, why); continue; }
    if (ctx.warm()) continue;
    Object c = o;
    c.asset = uint32_t(m);
    ctx.Emit(c);
    ++rep.drawn;
  }
  return true;
}

}  // namespace rdr_scene::rf
