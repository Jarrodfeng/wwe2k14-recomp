// S101 lane 133: the crowd's sign / foam-hand draws' factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME[,NAME] (fview): sign_wrong_entry, one_material, lightmap_stale, no_verify (crowd_props_core.h). The live path passes "".
#pragma once
#include "crowd_props_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeCrowdPropsOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  CrowdPropsOptions o;
  o.game_root = a.game_root;
  o.wait_for_load = !a.live;   // fview judges every frame of a capture
  if (!CropFaults::Parse(a.fault, o.faults, h.error)) return h;
  auto hook = std::make_unique<CrowdPropsOurs>(*a.renderer, o);
  CrowdPropsOurs* raw = hook.get();
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(std::move(r)); };
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.announce_arena = [raw](int arena) { raw->Announce(arena); };   // lane 271
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
