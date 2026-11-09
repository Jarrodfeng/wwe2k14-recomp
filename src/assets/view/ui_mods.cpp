// Session 98 (plan step 14): the MODS panel (see ui_mods.h). Portable: Dear ImGui + std; no Win32 / D3D.
#include "ui_mods.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include "../mods_lookup.h"
#include "../char_mods.h"
#include "imgui.h"
#include "json_lite.h"
#include "loader_seam.h"

namespace view {
namespace ui {

namespace ml = assets::mods_lookup;

void ModsPanel::Refresh() {
  scanned_ = true;
  ml::Reindex();  // the one directory read; everything else uses the cached index
  rows_.clear();
  banners_.clear();
  std::vector<int> game;
  std::string used;
  ViewerStageWrestlers("", game, used);  // the tron numbers the game's export cache (the launcher's tron previews) has pictures for
  std::map<int, Row> m;
  for (int id : game) m[id].id = id, m[id].game = true;
  if (!ml::Cfg().dir.empty())
    for (const ml::Entry& e : ml::ScanTrons()) {
      Row& r = m[e.id];
      r.id = e.id, r.yours = true, r.bink = e.bink, r.file = ml::U8(e.file.filename()), r.bytes = e.bytes;
    }
  for (auto& kv : m) rows_.push_back(kv.second);
  if (!ml::Cfg().dir.empty())
    for (const auto& [id, b] : ml::ScanBanners())
      banners_.push_back({id, b.mode == ml::BannerMode::kFile ? "file: " + ml::U8(b.file.filename()) : ml::BannerName(b.mode)});
}

std::string ModsPanel::SelSig(int id) const {
  const std::filesystem::path f = ml::TronFile(id);
  const ml::Banner b = ml::ReadBanner(id);
  return (f.empty() ? std::string() : ml::U8(f) + ml::FileSig(f)) + "|" + ml::BannerName(b.mode) + (b.file.empty() ? "" : ml::FileSig(b.file)) +
         (ml::Cfg().on ? "+" : "-");
}

void ModsPanel::SetNote(const std::string& reply, const std::string& ok_text) {
  if (reply.find("\"ok\":true") != std::string::npos) {
    note_ = ok_text;
    return;
  }
  const size_t p = reply.find("\"error\":\"");
  note_ = p == std::string::npos ? "failed" : reply.substr(p + 9, reply.find('"', p + 9) - (p + 9));
}

void ModsPanel::Select(int id, Queue* q) {
  sel_ = id;
  auto run = [this, id] {
    shown_sig_ = SelSig(id);
    const std::string r = Cmd("{\"cmd\":\"mods_select\",\"async\":true,\"tron\":" + std::to_string(id) + "}");
    if (r.find("\"ok\":true") == std::string::npos) SetNote(r, "");
    else note_.clear();
  };
  if (q) q->push_back(run);
  else run();
}

void ModsPanel::Poll() {
  try {  // runs outside the viewer's Safe(): an odd file name or a vanished folder must never end the process
    if (const int g = ml::DecodeGen().load(); g != seen_gen_) {  // a worker finished a decode: show the mod
      seen_gen_ = g;
      Cmd("{\"cmd\":\"mods_refresh\"}");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_poll_ < std::chrono::seconds(1)) return;
    last_poll_ = now;
    const std::string sig = ml::DirSig();
    if (sig != dir_sig_) {
      dir_sig_ = sig;
      Refresh();
    }
    if (sel_ >= 0 && SelSig(sel_) != shown_sig_) Select(sel_, nullptr);  // the shown tron's file (or its banner) changed: redraw the stage
  } catch (...) {
    note_ = "could not read the mods folder";
  }
}

void ModsPanel::Draw(float d, Queue& q) {
  ml::Config& C = ml::Cfg();
  if (!scanned_) Refresh();
  const ImVec4 kRed(1.f, 0.45f, 0.42f, 1.f), kDim(0.62f, 0.63f, 0.66f, 1.f);
  auto left = [&] { ImGui::SetCursorPosX(16 * d); };
  left();
  ImGui::PushTextWrapPos(0);
  if (C.dir.empty()) {
    if (!C.refused.empty()) ImGui::TextColored(kRed, "Mods folder refused: %s", C.refused.c_str());
    else ImGui::TextColored(kDim, "No mods folder. Start the viewer with --mods <folder> (the launcher passes it).");
  } else {
    ImGui::TextColored(kDim, "%s", ml::U8(C.dir).c_str());
  }
  ImGui::PopTextWrapPos();
  const bool have = !C.dir.empty();
  ImGui::BeginDisabled(!have);
  left();
  bool on = C.on;
  if (ImGui::Checkbox("USE MY MODS IN THE PREVIEW", &on))
    q.push_back([this, on] { Cmd(std::string("{\"cmd\":\"mods\",\"on\":") + (on ? "true" : "false") + "}"); });
  const bool sel_mine = sel_ >= 0 && !ml::TronFile(sel_).empty();
  left();
  const bool can_pick_video = bool(ml::Plat().pick_video);  // (the phone: no video picker / decoder in the viewer yet)
  ImGui::BeginDisabled(sel_ < 0 || !can_pick_video);
  if (ImGui::Button(can_pick_video ? "ADD TRON..." : "ADD TRON (PC only)")) {
    const int id = sel_;
    q.push_back([this, id] {  // the picker blocks: it runs here, outside the frame
      const std::string path = ml::Plat().pick_video ? ml::Plat().pick_video() : std::string();
      if (path.empty()) return;
      const std::string r = Cmd("{\"cmd\":\"mods_add\",\"async\":true,\"tron\":" + std::to_string(id) + ",\"file\":\"" + jl::Esc(path) + "\"}");
      SetNote(r, "Tron " + std::to_string(id) + " now plays your video");
    });
  }
  ImGui::EndDisabled();
  if (can_pick_video && sel_ < 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Select a tron in the list first");
  ImGui::SameLine();
  ImGui::BeginDisabled(!sel_mine);
  if (ImGui::Button("REMOVE")) confirm_remove_ = true;
  ImGui::EndDisabled();
  if (ml::Plat().open_folder) {  // (the phone has no folder window: the folder's path is shown above)
    left();
    if (ImGui::Button("OPEN FOLDER")) {
      q.push_back([] {
        if (ml::Plat().open_folder) ml::Plat().open_folder(ml::TronDir());
      });
    }
  }
  ImGui::EndDisabled();

  if (confirm_remove_) ImGui::OpenPopup("Remove my tron?"), confirm_remove_ = false;
  if (ImGui::BeginPopupModal("Remove my tron?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const std::string f = ml::U8(ml::TronFile(sel_).filename());
    ImGui::Text("Move %s to %s?", f.c_str(), ml::Plat().recycle_name.c_str());
    ImGui::TextDisabled("Only your file in the mods folder; the game's tron stays.");
    if (ImGui::Button("REMOVE")) {
      const int id = sel_;
      q.push_back([this, id] {
        const std::string r = Cmd("{\"cmd\":\"mods_remove\",\"tron\":" + std::to_string(id) + ",\"confirm\":true}");
        SetNote(r, "Tron " + std::to_string(id) + " mod moved to " + ml::Plat().recycle_name);
      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("CANCEL")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  int n_yours = 0;
  for (const Row& r : rows_) n_yours += r.yours;
  const float avail = ImGui::GetContentRegionAvail().y;
  const float list_h = std::max(84 * d, avail - 160 * d);
  left();
  const std::string head = "Trons (" + std::to_string(rows_.size()) + ", " + std::to_string(n_yours) + " yours)";
  if (ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::BeginChild("##mods_trons", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
      if (rows_.empty()) ImGui::TextDisabled("(no trons yet: build the launcher's tron previews)");
      for (const Row& r : rows_) {
        ImGui::PushID(r.id);
        char lab[96];
        std::snprintf(lab, sizeof lab, "%d", r.id);
        if (ImGui::Selectable(lab, sel_ == r.id, 0, ImVec2(46 * d, 0))) Select(r.id, &q);
        ImGui::SameLine();
        if (r.yours) {
          ImGui::TextColored(C.on ? kRed : kDim, "YOURS");
          ImGui::SameLine();
          ImGui::TextColored(kDim, "%s%s", r.file.c_str(), C.on ? "" : "  (off)");
        } else {
          ImGui::TextColored(kDim, "game");
        }
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    ImGui::TreePop();
  }
  left();
  const std::string bhead = "Banners (" + std::to_string(banners_.size()) + ")";
  if (ImGui::TreeNode(bhead.c_str())) {
    if (banners_.empty()) ImGui::TextDisabled("(none in your mods folder)");
    for (const auto& b : banners_) {
      ImGui::PushID(b.first + 100000);
      char lab[64];
      std::snprintf(lab, sizeof lab, "%d  %s", b.first, b.second.c_str());
      if (ImGui::Selectable(lab, false)) Select(b.first, &q);
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
  // Session 98: CHARACTERS (custom characters = community .pac files; copied to <mods>\characters, shown in the wrestler picker under CUSTOM)
  {
    std::vector<assets::char_mods::Entry> chars;
    try {
      chars = assets::char_mods::List(true);
    } catch (...) {
    }
    left();
    const std::string chead = "CHARACTERS (" + std::to_string(chars.size()) + ")";
    if (ImGui::TreeNodeEx(chead.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::BeginDisabled(!have);
      if (ImGui::Button("IMPORT CHARACTER...")) {
        q.push_back([this] {
          const std::string path = ml::Plat().pick_character ? ml::Plat().pick_character() : std::string();
          if (path.empty()) {
            if (!ml::Plat().pick_error.empty()) note_ = ml::Plat().pick_error;
            return;
          }
          const std::string r = Cmd("{\"cmd\":\"char_import\",\"path\":\"" + jl::Esc(path) + "\"}");
          std::string who;  // the imported names, from "imported":["a","b"]
          if (const size_t p = r.find("\"imported\":["); p != std::string::npos)
            for (char ch : r.substr(p + 12, r.find(']', p) - (p + 12))) {
              if (ch == '"' || ch == '\\') continue;
              who += ch;
              if (ch == ',') who += ' ';
            }
          std::string warn;  // "warnings":["name: text", ..] (an unusual skeleton, a zip cut short by a cap)
          if (const size_t p = r.find("\"warnings\":["); p != std::string::npos) {
            const std::string w = r.substr(p + 12, r.find(']', p) - (p + 12));
            for (size_t i = 0; i < w.size(); ++i) {
              if (w[i] == '\\' && i + 1 < w.size()) warn += w[++i];
              else if (w[i] == '"') continue;
              else warn += w[i] == ',' && i + 1 < w.size() && w[i + 1] == '"' ? std::string("; ") : std::string(1, w[i]);
            }
          }
          SetNote(r, (who.empty() ? std::string("Character") : who) + " imported: in the wrestler list under CUSTOM" +
                         (ml::Cfg().on ? "" : " (switch USE MY MODS on to see it)") + (warn.empty() ? "" : ". Warning: " + warn));
        });
      }
      if (!ml::Plat().pick_character && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("No file picker on this platform");
      ImGui::SameLine();
      ImGui::BeginDisabled(sel_char_.empty());
      if (ImGui::Button("REMOVE##char")) confirm_remove_char_ = true;
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      if (chars.empty()) ImGui::TextDisabled("(none yet: import a character .pac or a .zip that holds one)");
      for (const auto& c : chars) {
        ImGui::PushID(c.id);
        if (ImGui::Selectable(c.name.c_str(), sel_char_ == c.name)) sel_char_ = c.name;
        if (!C.on) {
          ImGui::SameLine();
          ImGui::TextColored(kDim, "(off)");
        }
        ImGui::PopID();
      }
      ImGui::TreePop();
    }
    if (confirm_remove_char_) ImGui::OpenPopup("Remove my character?"), confirm_remove_char_ = false;
    if (ImGui::BeginPopupModal("Remove my character?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Move %s to %s?", sel_char_.c_str(), ml::Plat().recycle_name.c_str());
      ImGui::TextDisabled("Only your imported copy in the mods folder.");
      if (ImGui::Button("REMOVE")) {
        const std::string nm = sel_char_;
        q.push_back([this, nm] {
          const std::string r = Cmd("{\"cmd\":\"char_remove\",\"name\":\"" + jl::Esc(nm) + "\",\"confirm\":true}");
          SetNote(r, nm + " moved to " + ml::Plat().recycle_name);
          sel_char_.clear();
        });
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("CANCEL")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
  for (const char* n : {"Attire textures", "Signage"}) {
    left();
    if (ImGui::TreeNode(n)) {
      ImGui::TextDisabled("Not supported by the game yet");
      ImGui::TreePop();
    }
  }
  if (sel_ >= 0) {  // what the preview shows for the selected tron
    left();
    ImGui::PushTextWrapPos(0);
    const std::filesystem::path f = ml::TronFile(sel_);
    if (f.empty()) ImGui::TextColored(kDim, "Tron %d: the game's own", sel_);
    else if (!C.on) ImGui::TextColored(kDim, "Tron %d: your %s is OFF, showing the game's", sel_, ml::U8(f.filename()).c_str());
    else if (ml::IsBink(f)) ImGui::TextColored(kDim, "Tron %d: your %s (Bink 2: no preview here)", sel_, ml::U8(f.filename()).c_str());
    else ImGui::TextColored(kRed, "Tron %d: your %s", sel_, ml::U8(f.filename()).c_str());
    ImGui::PopTextWrapPos();
  }
  if (!note_.empty()) {
    left();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "%s", note_.c_str());
    ImGui::PopTextWrapPos();
  }
}

}  // namespace ui
}  // namespace view
