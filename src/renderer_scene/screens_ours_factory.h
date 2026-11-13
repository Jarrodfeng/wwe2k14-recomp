// S101 lane 115: the stage screens' factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME[,NAME] (fview): the planted faults (colour_order, uv_halves_swapped, uv_sign, pair_swap, no_verify).
#pragma once
#include "screens_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeScreensOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  if (!a.renderer) {
    h.error = "no renderer";
    return h;
  }
  ScreensOursOptions o;
  o.game_root = a.game_root;
  o.verify_all = !a.live;
  std::string err;
  if (!ScreensFaults::Parse(a.fault, o.faults, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<ScreensOurs>(*a.renderer, o);
  ScreensOurs* raw = hook.get();
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(std::move(r)); };
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.announce_arena = [raw](int arena) { raw->Announce(arena); };   // lane 271
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
