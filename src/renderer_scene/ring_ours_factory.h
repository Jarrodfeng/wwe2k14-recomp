// S101 lane 70: the ring's factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME[,NAME] (fview): the planted faults of RING_PROMOTION_MAP.md section 4 (colour_order, uv_halves_swapped, uv_sign, skirt_tie, part_only_join, lod_pin, bone_zero, weight_word).
#pragma once
#include "ring_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeRingOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  if (!a.renderer) {
    h.error = "no renderer";
    return h;
  }
  RingOursOptions o;
  o.game_root = a.game_root;
  o.image_info = a.image_info;
  o.image_content = a.image_content;   // F1: the content check for slots 0 / 4 / 5 and the skirt tie; none = geometry only
  o.verify_all = !a.live;   // fview re-compares the vertex and index bytes on every draw; live on the first use of each blob key
  std::string err;
  if (!RingFaults::Parse(a.fault, o.faults, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<RingOurs>(*a.renderer, o);
  RingOurs* raw = hook.get();
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(std::move(r)); };
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.announce_arena = [raw](int arena) { raw->Announce(arena); };   // lane 271
  h.report = [raw]() { return raw->Report(); };
  h.wants_content = true;   // S101 lane 66: the content check reads args.image_content
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
