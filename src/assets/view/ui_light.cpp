// Session 98 (plan step 13, viewer side): the LIGHTING window. See ui_light.h. The window only talks to the viewer through the light_*
// commands (light_cmds.h); edits are queued (run by Ui::Pump before the frame), the list is refreshed after every change.
#include "ui_light.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include <chrono>

#include "imgui.h"
#include "../mods_lookup.h"
#include "json_lite.h"
#include "ui_importer.h"  // ReadBoolSetting / WriteBoolSetting (the launcher's settings file)

namespace view {
namespace ui {
namespace {

struct State {
  jl::Value listing;          // the last light_list reply
  bool have = false;
  bool dirty = true, refreshing = false;
  int arena = -2;
  bool entrance = false;
  int sel = -1;               // selected set id
  char dir[260] = "mods\\lighting";
  char file[260] = "";  // set to the current arena's file on the first arena (see LightingPanel)
  std::string mods_default;  // the <mods folder>\lighting the two boxes were last set to (Session 98 step 14)
  std::string status;
};
State& S() {
  static State s;
  return s;
}

std::string F(double v, int d = 4) {
  char b[48];
  std::snprintf(b, sizeof b, "%.*f", d, v);
  return b;
}
std::string Col3(const float* c) { return "[" + F(c[0]) + "," + F(c[1]) + "," + F(c[2]) + "]"; }
std::string Mode(bool entrance) { return std::string("\"mode\":\"") + (entrance ? "entrance" : "arena") + "\""; }

bool Ok(const std::string& r) { return r.find("\"ok\":true") != std::string::npos; }
std::string ErrOf(const std::string& r) {
  const size_t p = r.find("\"error\":\"");
  if (p == std::string::npos) return r.substr(0, 100);
  const size_t e = r.find('"', p + 9);
  return r.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9);
}

void V3(const jl::Value* v, float* o) {
  for (int i = 0; i < 3; ++i) o[i] = v && v->t == jl::Value::Arr && v->a.size() > size_t(i) ? float(v->a[i].n) : 0.f;
}

}  // namespace

void UseMyLighting();

void LightingPanel(const std::function<std::string(const std::string&)>& exec, std::vector<std::function<void()>>& queued, float d, int arena, bool entrance) {
  State& s = S();
  if (arena != s.arena || entrance != s.entrance) {
    if (arena != s.arena && arena >= 0) {  // the LOAD default is the current arena's file (in the SAVE folder)
      std::string dir = s.dir;
      while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
      std::snprintf(s.file, sizeof s.file, "%s\\arena_%02d.json", dir.c_str(), arena);
    }
    s.arena = arena, s.entrance = entrance, s.dirty = true, s.sel = -1, s.have = false;
  }
  if (s.dirty && !s.refreshing && arena >= 0) {
    s.refreshing = true;
    queued.push_back([exec, entrance] {
      State& st = S();
      const std::string reply = exec("{\"cmd\":\"light_list\"," + Mode(entrance) + "}");
      jl::Value v;
      std::string pe;
      st.have = false;
      if (jl::Parse(reply, v, pe) && v.t == jl::Value::Obj && Ok(reply)) st.listing = v, st.have = true;
      else st.status = ErrOf(reply);
      st.dirty = false, st.refreshing = false;
    });
  }
  // A command from a control: queued, then the list is refreshed.
  auto send = [&](const std::string& cmd, bool keep_status = false) {
    queued.push_back([exec, cmd, keep_status] {
      State& st = S();
      const std::string r = exec(cmd);
      if (!Ok(r)) st.status = ErrOf(r);
      else if (const size_t fp = r.find("\"file\":\""); fp != std::string::npos) {
        std::string f = r.substr(fp + 8, r.find('"', fp + 8) - fp - 8), plain;
        for (size_t i = 0; i < f.size(); ++i) plain += (f[i] == '\\' && i + 1 < f.size()) ? f[++i] : f[i];
        st.status = "wrote " + plain + (r.find("\"backup\":true") != std::string::npos ? " (old file kept as .bak)" : "");
      } else if (const size_t wp = r.find("\"warn\":\""); wp != std::string::npos) {
        st.status = r.substr(wp + 8, r.find('"', wp + 8) - wp - 8);
      } else if (!keep_status) st.status.clear();
      st.dirty = true;
    });
  };

  ImGui::BeginChild("##lighting", ImVec2(0, 0), false);  // fills the LIGHT tab of the sidebar
  UseMyLighting();
  ImGui::TextColored(ImVec4(0.84f, 0.09f, 0.12f, 1), "LIGHTING");
  ImGui::SameLine();
  if (arena < 0) {
    ImGui::TextDisabled("load an arena first");
    ImGui::EndChild();
    return;
  }
  const jl::Value& L = s.listing;
  const bool ab = !s.have || L.get("ab") == nullptr || L.get("ab")->b;
  ImGui::TextDisabled("%s  -  arena %d%s", entrance ? "ENTRANCE RIG (static per set, UNCONFIRMED in the game)" : "ARENA LIGHT KEY", arena,
                      s.have && L.get("key_enabled") && !L.get("key_enabled")->b ? "  (stage key OFF: edits still draw)" : "");
  if (!s.have) {
    ImGui::TextWrapped("%s", s.status.empty() ? "reading the lights..." : s.status.c_str());
    ImGui::EndChild();
    return;
  }
  ImGui::Separator();
  if (ImGui::Button(ab ? "SHOWING: EDITED (B)" : "SHOWING: ORIGINAL (B)")) send("{\"cmd\":\"light_ab\"," + Mode(entrance) + "}");
  ImGui::SameLine();
  if (ImGui::Button("RESET ALL")) send("{\"cmd\":\"light_reset\"," + Mode(entrance) + "}");
  if (ImGui::IsKeyPressed(ImGuiKey_B, false) && !ImGui::GetIO().WantTextInput) send("{\"cmd\":\"light_ab\"," + Mode(entrance) + "}");

  const jl::Value* sets = L.get("sets");
  if (!sets || sets->t != jl::Value::Arr || sets->a.empty()) {
    ImGui::TextDisabled("no light sets");
    ImGui::EndChild();
    return;
  }
  // set picker
  const jl::Value* cur = nullptr;
  for (const jl::Value& v : sets->a)
    if (int(v.num("id", -1)) == s.sel) cur = &v;
  if (!cur) {
    for (const jl::Value& v : sets->a)  // default: the first edited set, else 121, else the first used one
      if (v.get("edited") && v.get("edited")->b) { cur = &v; break; }
    if (!cur)
      for (const jl::Value& v : sets->a)
        if (int(v.num("id", -1)) == 121) cur = &v;
    if (!cur) cur = &sets->a[0];
    s.sel = int(cur->num("id", -1));
  }
  auto label = [](const jl::Value& v) {
    char b[160];
    const std::string nm = v.str("name");
    std::snprintf(b, sizeof b, "%d%s%s%s%s", int(v.num("id", 0)), nm.empty() ? "" : "  ", nm.c_str(), v.get("used") && !v.get("used")->b ? "  (not drawn here)" : "",
                  v.get("edited") && v.get("edited")->b ? "  [EDITED]" : "");
    return std::string(b);
  };
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##set", label(*cur).c_str())) {
    for (const jl::Value& v : sets->a) {
      const int id = int(v.num("id", 0));
      if (ImGui::Selectable(label(v).c_str(), id == s.sel)) s.sel = id;
    }
    ImGui::EndCombo();
  }
  const int id = s.sel;
  if (ImGui::Button("RESET THIS SET")) send("{\"cmd\":\"light_reset\",\"set\":" + std::to_string(id) + "," + Mode(entrance) + "}");

  ImGui::BeginChild("##lights", ImVec2(0, -150 * d), false);
  const jl::Value* lights = cur->get("lights");
  const std::string pre = "{\"cmd\":\"light_set\",\"set\":" + std::to_string(id) + "," + Mode(entrance) + ",";
  for (int k = 0; k < 5 && lights && lights->t == jl::Value::Arr && size_t(k) < lights->a.size(); ++k) {
    const jl::Value& lv = lights->a[k];
    if (lv.get("has") && !lv.get("has")->b) continue;
    ImGui::PushID(k);
    float col[3], hue[3];
    V3(lv.get("col"), col);
    float inten = std::max({col[0], col[1], col[2]});
    for (int i = 0; i < 3; ++i) hue[i] = inten > 1e-4f ? col[i] / inten : 1.f;
    ImGui::Text("LIGHT %d", k);
    ImGui::SameLine(80 * d);
    if (ImGui::ColorEdit3("##hue", hue, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueWheel)) {
      const float I = inten > 1e-4f ? inten : 1.f;
      const float nc[3] = {hue[0] * I, hue[1] * I, hue[2] * I};
      send(pre + "\"light\":" + std::to_string(k) + ",\"col\":" + Col3(nc) + "}");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * d);
    if (ImGui::DragFloat("##int", &inten, 0.01f, 0.f, 16.f, "intensity %.2f")) send(pre + "\"light\":" + std::to_string(k) + ",\"intensity\":" + F(inten) + "}");
    float yaw = float(lv.num("yaw", 0)), pitch = float(lv.num("pitch", 0));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##yaw", &yaw, -180.f, 180.f, "yaw %.1f deg")) send(pre + "\"light\":" + std::to_string(k) + ",\"yaw\":" + F(yaw, 2) + "}");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##pitch", &pitch, -180.f, 180.f, "pitch %.1f deg")) send(pre + "\"light\":" + std::to_string(k) + ",\"pitch\":" + F(pitch, 2) + "}");
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 4 * d));
  }
  const char* names[2] = {"sky", "ground"};
  for (int g = 0; g < 2; ++g) {
    ImGui::PushID(100 + g);
    float col[3], hue[3];
    V3(cur->get(names[g]), col);
    const float inten = std::max({col[0], col[1], col[2]});
    float inten_edit = inten;
    for (int i = 0; i < 3; ++i) hue[i] = inten > 1e-4f ? col[i] / inten : 1.f;
    ImGui::Text("%s", g ? "GROUND" : "SKY");
    ImGui::SameLine(80 * d);
    if (ImGui::ColorEdit3("##hue", hue, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueWheel)) {
      const float I = inten > 1e-4f ? inten : 1.f;
      const float nc[3] = {hue[0] * I, hue[1] * I, hue[2] * I};
      send(pre + "\"" + names[g] + "\":" + Col3(nc) + "}");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * d);
    if (ImGui::DragFloat("##int", &inten_edit, 0.01f, 0.f, 16.f, "intensity %.2f")) {
      const float I = inten > 1e-4f ? inten : 1.f, sc = inten_edit / I;
      const float h1[3] = {inten > 1e-4f ? col[0] * sc : inten_edit, inten > 1e-4f ? col[1] * sc : inten_edit, inten > 1e-4f ? col[2] * sc : inten_edit};
      send(pre + "\"" + names[g] + "\":" + Col3(h1) + "}");
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  ImGui::Separator();
  if (const std::string md = assets::mods_lookup::LightingDir(); !md.empty() && md != s.mods_default) {  // the mods folder (--mods): save / load there by default
    const bool untouched = s.mods_default.empty() ? std::strcmp(s.dir, "mods\\lighting") == 0 : s.mods_default == s.dir;
    s.mods_default = md;
    if (untouched) {
      std::snprintf(s.dir, sizeof s.dir, "%s", md.c_str());
      std::snprintf(s.file, sizeof s.file, "%s", (md + "/arena_" + (arena >= 0 ? (arena < 10 ? "0" : "") + std::to_string(arena) : std::string("05")) + ".json").c_str());
    }
  }
  ImGui::TextUnformatted(entrance ? "ENTRANCE MOD  (entrance_NNN.json)" : "ARENA MOD  (arena_NN.json)");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##dir", s.dir, sizeof s.dir);
  if (ImGui::Button("SAVE AS MOD")) send(std::string("{\"cmd\":\"light_save\",") + Mode(entrance) + ",\"dir\":\"" + jl::Esc(s.dir) + "\"}", false);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##file", s.file, sizeof s.file);
  if (ImGui::Button("LOAD MOD")) send(std::string("{\"cmd\":\"light_load\",") + Mode(entrance) + ",\"file\":\"" + jl::Esc(s.file) + "\"}", false);
  ImGui::SameLine();
  ImGui::TextDisabled("%d edited set(s), %d patch(es)", int(L.num("edited", 0)), int(L.num("patches", 0)));
  if (!s.status.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.45f, 1), "%s", s.status.c_str());
  ImGui::EndChild();
}

// USE MY LIGHTING: whether the game uses the saved lighting mods (the launcher's "lighting" key -> --wwe2k14_mods_lighting). Read
// again once a second (the launcher may change the file too); a click writes the key at once.
void UseMyLighting() {
  static bool on = false, have = false;
  static std::string err;
  static std::chrono::steady_clock::time_point last{};
  const std::string& file = LauncherSettingsFile();
  if (file.empty()) {
#if defined(_WIN32)
    ImGui::TextWrapped("USE MY LIGHTING: open the Arena Viewer from the launcher to switch your lighting on in the game.");
    ImGui::Separator();
#endif
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!have || now - last > std::chrono::seconds(1)) {
    bool v = false;
    if (ReadBoolSetting(file, "lighting", v)) on = v, have = true;
    last = now;
  }
  bool want = on;
  if (ImGui::Checkbox("USE MY LIGHTING IN THE GAME", &want)) {
    if (WriteBoolSetting(file, "lighting", want, err)) on = want, err.clear(), last = now;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(on ? "On: the light colours you saved for arenas and entrances are used in the game (separate from USE MY VIDEOS)."
                         : "Off: every original light is used in the game; your files stay in the mods folder.");
  if (!err.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.45f, 1), "Not saved: %s", err.c_str());
  ImGui::Separator();
}

}  // namespace ui
}  // namespace view
