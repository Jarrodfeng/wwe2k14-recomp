// S101 lane 84: the props' factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME[,NAME] (fview): the planted faults (colour_order, uv_halves_swapped, uv_sign, bone_zero, weight_word, counts_only_join, wrong_table_variant, steps_position_swap, all_mips, no_verify).
#pragma once
#include "props_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakePropsOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  if (!a.renderer) {
    h.error = "no renderer";
    return h;
  }
  PropsOursOptions o;
  o.game_root = a.game_root;
  o.image_info = a.image_info;
  o.image_content = a.image_content;   // the content check for slots 0 / 4 / 5 and the ties; none = geometry only
  o.verify_all = !a.live;
  std::string err;
  if (!PropsFaults::Parse(a.fault, o.faults, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<PropsOurs>(*a.renderer, o);
  PropsOurs* raw = hook.get();
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(std::move(r)); };
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.announce_arena = [raw](int arena) { raw->Announce(arena); };   // lane 271
  h.report = [raw]() { return raw->Report(); };
  h.wants_content = true;
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
