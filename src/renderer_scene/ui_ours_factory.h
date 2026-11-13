// S101 lane 66: the UI's factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME (fview) / the live path: the planted-fault names of UI_PROMOTION_MAP.md section 4 (first_texture, icon_as_font, rb_swap, key_shift, short_mips,
// names_stripped; stale_after_join is accepted and handled by fview's session, it changes the stream, not the hook). Offline (not live) the index may cache its hashes under
// the folder named by the environment variable WWE2K14_UI_INDEX_CACHE (the check scripts set it; nothing is cached live).
#pragma once
#include <cstdlib>
#include <filesystem>

#include "ui_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeUiOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  UiOursOptions o;
  o.game_root = a.game_root;
  o.live = a.live;
  o.image_content = a.image_content;
  o.quads = g_ui_quads;   // lane 148: --ui-quads / --wwe2k14_rdr_ui_quads
  if (o.quads && UiQuadFaults::Parse(a.fault, o.quad_faults)) {   // lane 148: a quad plant (wrong_uv_rect, no_blend, fade_alpha_ignored, half_pixel) leaves the join clean
  } else if (!UiFaults::Parse(a.fault, o.faults)) {
    h.error = "unknown --replace-fault '" + a.fault + "' for the ui (first_texture / wrong_texture, icon_as_font, rb_swap, key_shift, short_mips, names_stripped, stale_after_join, gpu_copy_after_join, no_view_check, no_deep, deep_wrong_arena, tier_boundary; each also as unchecked_NAME)";
    return h;
  }
  o.skip_bg_etc = a.low_ram;   // lane 126 (review F1.4)
  if (const char* cache = std::getenv("WWE2K14_UI_INDEX_CACHE")) o.cache_dir = cache;
  else if (a.live) {   // lane 126 (review F1.3): the hash cache live too (paths and hashes only, no pixels): %LOCALAPPDATA%\wwe2k14-recomp\ui_index; the phone has no folder here yet [U]
    if (const char* local = std::getenv("LOCALAPPDATA")) {
      std::error_code ec;
      const std::filesystem::path dir = std::filesystem::path(local) / "wwe2k14-recomp" / "ui_index";
      std::filesystem::create_directories(dir, ec);
      if (!ec) o.cache_dir = dir.string();
    }
  }
  auto hook = std::make_unique<UiOurs>(*a.renderer, o);
  UiOurs* raw = hook.get();
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(std::move(r)); };   // lane 126: the arena for the second tier
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.report = [raw]() { return raw->Report(); };
  h.wants_content = true;   // S101 lane 66: the content check reads args.image_content
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
