// lane 139: the plane shadow chain's factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault plane.NAME[,plane.NAME] (fview): inv_identity (plane_inv_identity), camera_stale (plane_camera_stale), eye_shift (eye_point_shift), no_verify.
#pragma once


namespace rdr_scene {

ReplaceHookHandle MakePlaneShadowHook(const ReplaceFactoryArgs& args);   // defined in plane_shadow_ours.cpp (a declaration, not an include: that header includes replace_factories.h)
inline ReplaceHookHandle MakePlaneShadowOurs(const ReplaceFactoryArgs& a) { return MakePlaneShadowHook(a); }

}  // namespace rdr_scene
