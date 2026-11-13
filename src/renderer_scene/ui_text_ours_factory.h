// S101 lane 89 (UT): the game's text drawn by us, the factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME (fview) / the live path: advance_c2, kern_off, wrong_font, run_shift (UiTextFaults); `unchecked_NAME` draws the planted layout (the guard counts only).
#pragma once
#include "../renderer/problem.h"
#include "ui_text_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeUiTextOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  UiTextOptions o;
  o.game_root = a.game_root;
  o.live = a.live;
  o.log = [](const std::string& s) { rdr::Log("%s", s.c_str()); };
  if (!UiTextFaults::Parse(a.fault, o.faults)) {
    h.error = "unknown --replace-fault '" + a.fault + "' for ui_text (advance_c2, kern_off, wrong_font, run_shift, each also as unchecked_NAME)";
    return h;
  }
  auto hook = std::make_unique<UiTextOurs>(o);
  UiTextOurs* raw = hook.get();
  h.set_game_root = [raw](const std::string& root) { raw->SetGameRoot(root); };
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
