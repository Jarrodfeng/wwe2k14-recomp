// Session 98 (plan step 16, "click to inspect"): the INSPECT panel of the Arena Viewer's sidebar. Pure Dear ImGui + std (no Win32 / D3D),
// drawn by ui.cpp's Sidebar in place of the old placeholder. It shows one PartInfo (pick.h): the friendly name large, the literal asset
// name and source under it, category / light / placement / motion, texture thumbnails (click -> that texture in the ASSETS tab), the stage
// movies, and the mod stubs (SWAP TEXTURE / SHOW IN MODS: they only report what they would do until the mod workbench exists).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "pick.h"
#include "ui_backend.h"

namespace view {
namespace ui {

struct InspectDraw {
  float d = 1.f;  // dpi scale
  ImU32 red = IM_COL32(214, 22, 30, 255);
  std::function<void(ImDrawList*, ImVec2, float, ImU32, const char*)> text;  // the menu face with a shadow (px, colour)
  std::function<float(float, const char*)> width;                            // its width at px
  std::function<void(int, int)> open_texture;                                // model, slot -> the ASSETS tab, that texture
  std::function<void(const std::string&)> open_movie;                        // the ASSETS tab, VIDEOS
  std::function<void(const std::string&)> mod;                               // "swap_texture" / "show_in_mods" (stubs)
  std::function<void()> deselect;
};

// Draws at the current ImGui cursor, `room` pixels wide. `info` may be null / invalid (then a hint is shown).
void DrawInspect(const PartInfo* info, const std::vector<ModelMeta>& models, const InspectDraw& D, float room);

}  // namespace ui
}  // namespace view
