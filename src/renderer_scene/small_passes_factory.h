// S101 lane 135: the small passes' factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault small.NAME (fview): sweat_no_prev, slot_null, prelight_wrong_target_size, no_verify (small_passes_ours.h). The live path passes "".
#pragma once
#include "small_passes_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeSmallPassesOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  SmallFaults f;
  if (!SmallFaults::Parse(a.fault, f, h.error)) return h;
  auto hook = std::make_unique<SmallPassesOurs>(*a.renderer, f, a.live);
  SmallPassesOurs* raw = hook.get();
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);   // needs no scene record, game root or image content
  return h;
}

}  // namespace rdr_scene
