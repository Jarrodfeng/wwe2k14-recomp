// lane 151: the rim light map's reductions-and-blur factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault rimchain.NAME[,rimchain.NAME] (fview): reduction_wrong_size, blur_no_input, blur_input_null, rim_reader_stale, unmasked (experiment), no_verify.
#pragma once

namespace rdr_scene {

ReplaceHookHandle MakeRimChainHook(const ReplaceFactoryArgs& args);   // defined in rim_chain_ours.cpp (a declaration, not an include: that header includes replace_factories.h)
inline ReplaceHookHandle MakeRimChainOurs(const ReplaceFactoryArgs& a) { return MakeRimChainHook(a); }

}  // namespace rdr_scene
