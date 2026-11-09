// Session 98 (plan step 13, viewer side): the LIGHTING panel of the Arena Viewer UI (ui_light.cpp). A button in the RUNTIME sidebar
// opens a window beside it: the light sets of the loaded arena (or the entrance rig), per-light colour / intensity / direction,
// sky and ground, A/B, reset, SAVE AS MOD / LOAD. Every change goes through the viewer's light_* commands (light_cmds.h).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace view {
namespace ui {

// Whether the LIGHTING window is open (also set by the light_panel command).
inline bool& LightingPanelOpen() {
  static bool open = false;
  return open;
}

// The launcher's settings file (--settings-file; "" = not started from the launcher): the LIGHT tab's USE MY LIGHTING switch is its
// "lighting" key (Session 98, Jarrod: the switch belongs to the Arena Viewer, not the launcher's MODS page).
inline std::string& LauncherSettingsFile() {
  static std::string f;
  return f;
}

// Called from the sidebar while it is built: draws the opener button and, when open, the window. `queued` = the UI's action queue
// (run by Ui::Pump before the frame); `arena` = the shown arena (-1 none); `entrance` = the entrance look is on.
void LightingPanel(const std::function<std::string(const std::string&)>& exec, std::vector<std::function<void()>>& queued, float dpi, int arena,
                   bool entrance);

}  // namespace ui
}  // namespace view
