// Session 81 (Jarrod): the update dialog shows the release notes as GitHub writes them (markdown), drawn with Dear
// ImGui. A small subset, enough for our changelog style: "# " headings, "- " / "* " bullets, "> " quotes, ``` code
// blocks, "---" rules, and inside a line **bold**, `code` and [text](https://link). Anything else shows as plain text.
// Shared by the PC and the Android launcher (both draw with ImGui); links go through the caller's opener, which only
// ever opens https:// addresses.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

namespace launcher {

struct MdSpan {
  std::string text, url;
  int style = 0;  // 0 plain, 1 bold, 2 code, 3 link
};

inline std::vector<MdSpan> MdInline(const std::string& s) {
  std::vector<MdSpan> out;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) out.push_back({cur, {}, 0}), cur.clear();
  };
  for (size_t i = 0; i < s.size();) {
    if (s.compare(i, 2, "**") == 0) {
      const size_t e = s.find("**", i + 2);
      if (e != std::string::npos && e > i + 2) {
        flush();
        out.push_back({s.substr(i + 2, e - i - 2), {}, 1});
        i = e + 2;
        continue;
      }
    }
    if (s[i] == '`') {
      const size_t e = s.find('`', i + 1);
      if (e != std::string::npos && e > i + 1) {
        flush();
        out.push_back({s.substr(i + 1, e - i - 1), {}, 2});
        i = e + 1;
        continue;
      }
    }
    if (s[i] == '[') {
      const size_t mid = s.find("](", i + 1);
      const size_t end = mid == std::string::npos ? mid : s.find(')', mid + 2);
      if (end != std::string::npos) {
        flush();
        out.push_back({s.substr(i + 1, mid - i - 1), s.substr(mid + 2, end - mid - 2), 3});
        i = end + 1;
        continue;
      }
    }
    cur += s[i++];
  }
  flush();
  return out;
}

// Draws `md` at the cursor, wrapped at `wrap_w`, and leaves the cursor below it.
inline void RenderMarkdown(const std::string& md, float wrap_w, float dpi,
                           const std::function<void(const std::string&)>& open_link) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float lh = ImGui::GetTextLineHeight(), space = ImGui::CalcTextSize(" ").x;
  const ImU32 c_plain = ImGui::GetColorU32(ImGuiCol_Text), c_bold = IM_COL32(255, 255, 255, 255),
              c_code = IM_COL32(240, 178, 130, 255), c_link = IM_COL32(110, 178, 255, 255),
              c_quote = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  float y = 0;
  bool fence = false;
  size_t pos = 0;
  while (pos <= md.size()) {
    size_t nl = md.find('\n', pos);
    if (nl == std::string::npos) nl = md.size();
    std::string line = md.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (line.rfind("```", 0) == 0) {
      fence = !fence;
      continue;
    }
    if (fence) {
      dl->AddText(ImVec2(origin.x + 12 * dpi, origin.y + y), c_code, line.c_str());
      y += lh;
      continue;
    }
    if (line.find_first_not_of(" \t") == std::string::npos) {
      y += lh * 0.5f;
      continue;
    }
    if (line.rfind("---", 0) == 0 || line.rfind("***", 0) == 0) {
      dl->AddLine(ImVec2(origin.x, origin.y + y + lh * 0.5f), ImVec2(origin.x + wrap_w, origin.y + y + lh * 0.5f),
                  IM_COL32(120, 120, 120, 160));
      y += lh;
      continue;
    }

    float indent = 0;
    bool heading = false, quote = false;
    size_t first = line.find_first_not_of(' ');
    std::string body = line.substr(first);
    if (body.rfind("#", 0) == 0) {
      const size_t h = body.find_first_not_of('#');
      if (h != std::string::npos && body[h] == ' ') body = body.substr(h + 1), heading = true;
      if (heading) y += lh * 0.3f;
    } else if (body.rfind("- ", 0) == 0 || body.rfind("* ", 0) == 0) {
      indent = 16 * dpi + float(first) * 6 * dpi;
      dl->AddText(ImVec2(origin.x + indent - 12 * dpi, origin.y + y), c_plain, "-");
      body = body.substr(2);
    } else if (body.rfind("> ", 0) == 0) {
      indent = 14 * dpi, quote = true, body = body.substr(2);
      dl->AddLine(ImVec2(origin.x + 4 * dpi, origin.y + y), ImVec2(origin.x + 4 * dpi, origin.y + y + lh),
                  IM_COL32(150, 150, 150, 200), 2 * dpi);
    }

    float x = indent;
    std::vector<MdSpan> spans = MdInline(body);
    if (heading) {
      for (auto& sp : spans) sp.style = sp.style == 3 ? 3 : 1;
    }
    for (const MdSpan& sp : spans) {
      const ImU32 col = sp.style == 1 ? c_bold : sp.style == 2 ? c_code : sp.style == 3 ? c_link : quote ? c_quote : c_plain;
      size_t i = 0;
      const std::string& t = sp.text;
      while (i < t.size()) {
        const size_t sp_at = t.find(' ', i);
        const size_t end = sp_at == std::string::npos ? t.size() : sp_at;
        const std::string word = t.substr(i, end - i);
        i = end < t.size() ? end + 1 : end;
        const float w = ImGui::CalcTextSize(word.c_str()).x;
        if (x > indent && x + w > wrap_w) x = indent, y += lh;
        if (!word.empty()) {
          const ImVec2 at(origin.x + x, origin.y + y);
          dl->AddText(at, col, word.c_str());
          if (sp.style == 1) dl->AddText(ImVec2(at.x + 0.8f * dpi, at.y), col, word.c_str());
          if (sp.style == 3) {
            const ImVec2 a = at, b(at.x + w, at.y + lh);
            const bool over = ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered();
            if (over) dl->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), col);
            if (over && ImGui::IsMouseClicked(0) && open_link) open_link(sp.url);
          }
          x += w;
        }
        if (sp_at != std::string::npos) x += space;  // a space only where the text had one (**bold**, stays tight)
      }
    }
    y += lh + (heading ? lh * 0.2f : 2 * dpi);
  }
  ImGui::Dummy(ImVec2(wrap_w, y));
}

}  // namespace launcher
