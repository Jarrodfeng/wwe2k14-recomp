// S101 lane 40: the arena's factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME (fview) / the live path: the planted-fault names of ARENA_PROMOTION_MAP.md section 4 plus "uv_abs_u" (not a fault: the A/B for the
// stream's |u|, ARENA_OURS.md).
#pragma once
#include "arena_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeArenaOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  ArenaOursOptions o;
  o.game_root = a.game_root;
  o.image_info = a.image_info;
  o.verify_content = !a.live;  // fview re-compares the positions on every draw
  const std::string& f = a.fault;
  if (f == "vertex_swizzle") o.faults.vertex_swizzle = true;
  else if (f == "no_mirror") o.faults.no_mirror = true;
  else if (f == "first_texture") o.faults.first_texture = true;
  else if (f == "no_trim_window") o.faults.no_trim_window = true;   // lane 243
  else if (f == "window_shift") o.faults.window_shift = true;       // lane 243
  else if (f == "strict_texture") o.faults.strict_texture = true;   // lane 142
  else if (f == "uv_shift") o.faults.uv_shift = true;   // lane 142
  else if (f == "viewer_space") o.faults.viewer_space = true;
  else if (f == "file_lightmap") o.faults.file_lightmap = true;
  else if (f == "join_shift") o.faults.join_shift = true;
  else if (f == "no_group_rule") o.faults.no_group_rule = true;
  else if (f == "winding_flip") o.faults.winding_flip = true;
  else if (f == "bone_off_by_one") o.faults.bone_off_by_one = true;
  else if (f == "bone_zero") o.faults.bone_zero = true;
  else if (f == "uv_abs_u") o.uv_abs_u = true;
  else if (f == "content_no_soft") o.faults.content_no_soft = true;   // S101 thin vswap
  else if (!f.empty()) {
    h.error = "unknown --replace-fault '" + f + "' for the arena (vertex_swizzle, no_mirror, first_texture, viewer_space, file_lightmap, join_shift, no_group_rule, winding_flip, bone_off_by_one, bone_zero, uv_abs_u, content_no_soft, strict_texture, uv_shift, no_trim_window, window_shift)";
    return h;
  }
  auto hook = std::make_unique<ArenaOurs>(*a.renderer, o);
  ArenaOurs* raw = hook.get();
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(std::move(r)); };
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.announce_arena = [raw](int arena) { raw->Announce(arena); };   // lane 271
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
