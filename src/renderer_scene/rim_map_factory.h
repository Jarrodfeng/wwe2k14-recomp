// lane 124: the rim light map's factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault rim.NAME[,rim.NAME] (fview): no_pose, first_object, wrong_target, no_verify (and the writer's: fetch_order, weights_as_fill, xdword_colour, bone_zero).
#pragma once


namespace rdr_scene {

ReplaceHookHandle MakeRimMapHook(const ReplaceFactoryArgs& args);   // defined in rim_map_ours.cpp (a declaration, not an include: that header includes replace_factories.h)
inline ReplaceHookHandle MakeRimMapOurs(const ReplaceFactoryArgs& a) { return MakeRimMapHook(a); }

}  // namespace rdr_scene
