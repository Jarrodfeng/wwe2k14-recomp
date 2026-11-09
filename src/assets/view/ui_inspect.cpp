// Session 98 (plan step 16): the INSPECT panel body. See ui_inspect.h.
#include "ui_inspect.h"

#include <algorithm>
#include <cstdio>

namespace view {
namespace ui {
namespace {

// Splits `s` at spaces into lines no wider than `room` at `px` (a single long word keeps its own line).
std::vector<std::string> WrapLines(const InspectDraw& D, const std::string& s, float px, float room, size_t max_lines) {
  std::vector<std::string> lines;
  std::string cur;
  size_t i = 0;
  while (i < s.size()) {
    size_t e = s.find(' ', i);
    if (e == std::string::npos) e = s.size();
    const std::string word = s.substr(i, e - i);
    const std::string next = cur.empty() ? word : cur + " " + word;
    if (!cur.empty() && D.width(px, next.c_str()) > room) {
      lines.push_back(cur);
      cur = word;
    } else {
      cur = next;
    }
    i = e + 1;
  }
  if (!cur.empty()) lines.push_back(cur);
  while (lines.size() > max_lines) {  // (too long even wrapped: the tail joins the last line)
    lines[max_lines - 1] += " " + lines.back();
    lines.pop_back();
  }
  return lines;
}

void Row(const char* label, const std::string& value, float d) {
  if (value.empty()) return;
  ImGui::TextDisabled("%s", label);
  ImGui::SameLine(88 * d);
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(value.c_str());
  ImGui::PopTextWrapPos();
}

}  // namespace

void DrawInspect(const PartInfo* info, const std::vector<ModelMeta>& models, const InspectDraw& D, float room) {
  const float d = D.d;
  if (!info || !info->valid) {
    ImGui::TextDisabled("Click a part of the arena");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("A wrestler, a crowd member, the ring, the steps, the desk, a screen. Hover shows a faint glow; the selected part gets a red outline.");
    ImGui::PopTextWrapPos();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // the friendly name, large (up to three lines)
  const float px = 23 * d;
  for (const std::string& line : WrapLines(D, info->friendly, px, room, 3)) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    D.text(dl, p, px, IM_COL32_WHITE, line.c_str());
    ImGui::Dummy(ImVec2(room, px + 2 * d));
  }
  // the literal asset name as a subheading, then the source
  ImGui::PushTextWrapPos(0);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.62f, 0.64f, 1));
  ImGui::TextUnformatted(info->literal.c_str());
  ImGui::PopStyleColor();
  ImGui::TextDisabled("%s", info->source.c_str());
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, 4 * d));
  Row("CATEGORY", info->category, d);
  Row("PIECE", info->mesh_note, d);

  if (!info->textures.empty()) {
    ImGui::Dummy(ImVec2(0, 4 * d));
    ImGui::TextDisabled("TEXTURES  (click: open in ASSETS)");
    const float thumb = 54 * d, gap = 6 * d;
    const int per_row = std::max(1, (int)((room + gap) / (thumb + gap)));
    for (size_t i = 0; i < info->textures.size(); i++) {
      const PartTexRef& t = info->textures[i];
      if (i % (size_t)per_row) ImGui::SameLine(0, gap);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::PushID((int)i);
      const bool clicked = ImGui::InvisibleButton("##tex", ImVec2(thumb, thumb));
      ImGui::PopID();
      const bool hover = ImGui::IsItemHovered();
      ImTextureID id = 0;
      if (t.model >= 0 && t.model < (int)models.size() && t.slot >= 0 && t.slot < (int)models[(size_t)t.model].textures.size())
        id = models[(size_t)t.model].textures[(size_t)t.slot].id;
      const ImVec2 a = p, e(p.x + thumb, p.y + thumb);
      dl->AddRectFilled(a, e, IM_COL32(30, 31, 38, 255));
      if (id) dl->AddImage(ImTextureRef(id), ImVec2(a.x + 2, a.y + 2), ImVec2(e.x - 2, e.y - 2));
      dl->AddRect(a, e, hover ? D.red : IM_COL32(90, 40, 44, 255), 0, 0, hover ? 2.f : 1.f);
      if (hover) {
        ImGui::BeginTooltip();
        ImGui::Text("%s: %s", t.role.c_str(), t.name.c_str());
        ImGui::TextDisabled("%u x %u", t.w, t.h);
        ImGui::EndTooltip();
      }
      if (clicked && D.open_texture) D.open_texture(t.model, t.slot);
    }
    for (size_t i = 0; i < info->textures.size(); i++)  // the names, small, under the thumbnails
      ImGui::TextDisabled("%s  %s", info->textures[i].role.c_str(), info->textures[i].name.c_str());
    ImGui::Dummy(ImVec2(0, 2 * d));
  }
  Row("LIGHT", info->light, d);
  Row("MOTION", info->motion, d);
  Row("PLACEMENT", info->placement, d);
  {
    char b[96];
    std::snprintf(b, sizeof b, "%.0f  %.0f  %.0f   turn %.0f deg", info->pos[0], info->pos[1], info->pos[2], info->turn_deg);
    Row("POSITION", b, d);
  }
  Row("NODE", info->node, d);
  Row("SKIN", info->palette, d);
  Row("MATERIAL", info->shader, d);
  if (!info->movies.empty()) {
    ImGui::Dummy(ImVec2(0, 4 * d));
    ImGui::TextDisabled("MOVIES  (stage media)");
    for (size_t i = 0; i < info->movies.size(); i++) {
      ImGui::PushID((int)(1000 + i));
      if (ImGui::Button(info->movies[i].c_str()) && D.open_movie) D.open_movie(info->movies[i]);
      ImGui::PopID();
    }
  }
  ImGui::Dummy(ImVec2(0, 6 * d));
  if (ImGui::Button("SWAP TEXTURE") && D.mod) D.mod("swap_texture");
  ImGui::SameLine();
  if (ImGui::Button("SHOW IN MODS") && D.mod) D.mod("show_in_mods");
  if (D.deselect && ImGui::Button("CLEAR SELECTION")) D.deselect();
}

}  // namespace ui
}  // namespace view
