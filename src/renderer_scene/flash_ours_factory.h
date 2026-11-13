// S101 lane 68: the crowd flashes' factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME (fview): no_depth, first_arena, wrong_arena, count_off, all_mips (flash_ours.h). The live path passes "".
#pragma once
#include "flash_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeFlashOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  FlashOursOptions o;
  o.game_root = a.game_root;
  o.image_info = a.image_info;
  o.image_content = a.image_content;
  o.verify_content = !a.live;  // fview re-compares every vertex on every draw
  const std::string& f = a.fault;
  if (f == "no_depth") o.faults.no_depth = true;
  else if (f == "first_arena") o.faults.first_arena = true;
  else if (f == "wrong_arena") o.faults.wrong_arena = true;
  else if (f == "count_off") o.faults.count_off = true;
  else if (f == "all_mips") o.faults.all_mips = true;
  else if (!f.empty()) {
    h.error = "unknown --replace-fault '" + f + "' for the flashes (no_depth, first_arena, wrong_arena, count_off, all_mips)";
    return h;
  }
  auto hook = std::make_unique<FlashOurs>(*a.renderer, o);
  FlashOurs* raw = hook.get();
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(std::move(r)); };
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.announce_arena = [raw](int arena) { raw->Announce(arena); };   // lane 271
  h.report = [raw]() { return raw->Report(); };
  h.wants_content = true;   // S101 lane 66: the content check reads args.image_content
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
