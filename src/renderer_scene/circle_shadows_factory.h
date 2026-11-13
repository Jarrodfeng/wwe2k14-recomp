// lane 136: the circle blob shadows' factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault circles.NAME[,circles.NAME] (fview): bind_pose (circle_bind_pose), join_by_index (circle_join_by_index), no_fade (circle_no_fade), no_verify.
#pragma once


namespace rdr_scene {

ReplaceHookHandle MakeCirclesHook(const ReplaceFactoryArgs& args);   // defined in circle_shadows_ours.cpp (a declaration, not an include: that header includes replace_factories.h)
inline ReplaceHookHandle MakeCirclesOurs(const ReplaceFactoryArgs& a) { return MakeCirclesHook(a); }

}  // namespace rdr_scene
