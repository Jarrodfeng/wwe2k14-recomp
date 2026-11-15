// S102 lane R24-F (docs\renderer\GPU_STAMPS.md "Group stamps"): which draw GROUP (src\renderer\group_stamps.h GroupId) a draw belongs to, for the group stamps inside a colour / frame pass.
// From the draw's origin alone (class, call site, pass site: names.h, crowd_hook.h, draw_replace_hook.h). The binder refines a model part by what our hooks drew it as
// (binder.h GroupOfFate: arena / wrestler / ring / props / flash / screens), because the arena, the wrestlers and the props share the model-part sites (the object, not
// the site, tells them apart, and only the hooks know the object). No game includes; a constants-only dependency on the origin tables.
#pragma once
#include <cstdint>

#include "../renderer/group_stamps.h"
#include "crowd_hook.h"
#include "draw_replace_hook.h"
#include "names.h"

namespace rdr_adapter {

inline uint8_t GroupOfOrigin(const DrawOrigin* o) {
  if (!o) return rdr::kGrpUnnamed;
  switch (o->klass) {
    case DrawClass::Unnamed: return rdr::kGrpUnnamed;
    case DrawClass::ModelPart:
      if (o->pass_site == kPlaneShadowSite) return rdr::kGrpPlaneShadow;
      if (o->pass_site == kModelPartColourSite) return rdr::kGrpModelColour;
      if (o->pass_site == kModelPartMirrorSite) return rdr::kGrpModelMirror;
      return rdr::kGrpModelOther;
    case DrawClass::Crowd:
      return o->call_site == kCrowdCardsSite ? rdr::kGrpCrowdCards : o->call_site == kCrowdFloorSite ? rdr::kGrpCrowdFloor : rdr::kGrpCrowd;
    case DrawClass::Sprite: return rdr::kGrpSprite;
    case DrawClass::Text: return rdr::kGrpText;
    case DrawClass::Movie: return rdr::kGrpMovie;
    case DrawClass::Particle: return rdr::kGrpParticle;
    case DrawClass::Shadow: return o->pass_site == kPlaneShadowSite ? rdr::kGrpPlaneShadow : rdr::kGrpShadow;
    case DrawClass::Post: return rdr::kGrpPost;
    case DrawClass::Clear: return rdr::kGrpClear;
    case DrawClass::Other:
    case DrawClass::Count: break;
  }
  return rdr::kGrpOther;
}

}  // namespace rdr_adapter
