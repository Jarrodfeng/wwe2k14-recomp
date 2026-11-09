// Session 98 (plan step 12): the CAMERA PATH panel of the Arena Viewer UI (called from ui.cpp's sidebar). Pure Dear ImGui + std.
// It reads the viewer's PathUi (campath.h) and changes the scene only through the viewer's JSON commands (path_*, render, ...).
#pragma once
#include <functional>
#include <string>

#include "campath.h"

namespace view {
namespace ui {

void PathPanel(float dpi, const std::function<std::string(const std::string&)>& exec, PathUi* pu);

}  // namespace ui
}  // namespace view
