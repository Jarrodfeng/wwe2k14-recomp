// S101 lane 99: the shadow casters' factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault shadow.NAME[,shadow.NAME] (fview): palette_from_colour, fetch_order, weights_as_fill, xdword_colour, bone_zero, proxy_part_shift, no_verify.
#pragma once


namespace rdr_scene {

ReplaceHookHandle MakeShadowCastersHook(const ReplaceFactoryArgs& args);   // defined in shadow_casters_ours.cpp (a declaration, not an include: that header includes replace_factories.h)
inline ReplaceHookHandle MakeShadowCastersOurs(const ReplaceFactoryArgs& a) { return MakeShadowCastersHook(a); }

}  // namespace rdr_scene
