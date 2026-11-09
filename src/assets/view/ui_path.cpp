// Session 98 (plan step 12): see ui_path.h.
#include "ui_path.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "json_lite.h"

namespace view {
namespace ui {
namespace {

struct PanelState {
  char name[96] = "path1";
  char out[260] = "out\\renders\\path1";
  int w = 1920, h = 1080, fps = 30, ss = 1, samples = 1;
  float blur = 0.0f;
  bool mp4 = false, scene = true;
  std::string msg;
};
PanelState& St() {
  static PanelState s;
  return s;
}

std::string Fx(double v) {
  char b[48];
  snprintf(b, sizeof b, "%.6g", v);
  return b;
}
std::string Reply(const std::string& r) {  // the reply's "error" text, or "" when ok
  jl::Value v;
  std::string e;
  if (!jl::Parse(r, v, e)) return "bad reply";
  const jl::Value* ok = v.get("ok");
  if (ok && ok->t == jl::Value::Bool && !ok->b) return v.str("error", "failed");
  return "";
}

}  // namespace

void PathPanel(float d, const std::function<std::string(const std::string&)>& exec, PathUi* pu) {
  (void)d;
  PanelState& st = St();
  if (!pu || !exec) {
    ImGui::TextDisabled("(not available)");
    return;
  }
  auto run = [&](const std::string& c) {
    const std::string e = Reply(exec(c));
    if (!e.empty()) st.msg = e;
    return e.empty();
  };
  CamPath& path = pu->path;
  const bool busy = pu->rendering;
  if (ImGui::TreeNodeEx("Camera path", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("ADD KEY  (K)")) run("{\"cmd\":\"path_add\",\"t\":" + Fx(pu->time) + "}");
    ImGui::SameLine();
    if (ImGui::Button(pu->playing ? "PAUSE" : "PLAY")) run(std::string("{\"cmd\":\"path_play\",\"on\":") + (pu->playing ? "false" : "true") + "}");
    ImGui::SameLine();
    if (ImGui::Button("STOP")) {
      run("{\"cmd\":\"path_play\",\"on\":false}");
      if (!path.keys.empty()) run("{\"cmd\":\"path_time\",\"t\":" + Fx(path.Start()) + "}");
    }
    bool loop = path.loop, link = pu->link;
    if (ImGui::Checkbox("LOOP", &loop)) run(std::string("{\"cmd\":\"path_set\",\"loop\":") + (loop ? "true" : "false") + "}");
    ImGui::SameLine();
    if (ImGui::Checkbox("LINK SCENE CLOCK", &link)) run(std::string("{\"cmd\":\"path_set\",\"link\":") + (link ? "true" : "false") + "}");
    const float lo = (float)path.Start(), hi = std::max((float)path.End(), lo + 1.0f);
    float t = (float)pu->time;
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderFloat("##path_t", &t, lo, hi, "%.2f s") && !path.keys.empty()) run("{\"cmd\":\"path_time\",\"t\":" + Fx(t) + "}");
    ImGui::PopItemWidth();
    ImGui::EndDisabled();

    if (ImGui::BeginTable("##keys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22);
      ImGui::TableSetupColumn("TIME");
      ImGui::TableSetupColumn("FOV");
      ImGui::TableSetupColumn("EASE");
      ImGui::TableHeadersRow();
      for (int i = 0; i < (int)path.keys.size(); i++) {
        const CamKey& k = path.keys[(size_t)i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char lab[32];
        snprintf(lab, sizeof lab, "%d##key%d", i + 1, i);
        if (ImGui::Selectable(lab, pu->selected == i, ImGuiSelectableFlags_SpanAllColumns)) pu->selected = i;
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.2f s", k.t);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.0f", k.p.fov);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.1f / %.1f", k.ease_in, k.ease_out);
      }
      ImGui::EndTable();
    }
    if (path.keys.empty()) ImGui::TextDisabled("No keys yet: fly the camera, press K.");
    const int sel = pu->selected;
    if (sel >= 0 && sel < (int)path.keys.size() && !busy) {
      CamKey k = path.keys[(size_t)sel];
      bool changed = false;
      float kt = (float)k.t;
      ImGui::TextUnformatted("KEY TIME");
      ImGui::PushItemWidth(-1);
      if (ImGui::DragFloat("##k_time", &kt, 0.01f, 0.0f, 3600.0f, "%.2f s")) { k.t = kt; changed = true; }
      ImGui::TextUnformatted("EASE IN / OUT");
      float e2[2] = {k.ease_in, k.ease_out};
      if (ImGui::SliderFloat2("##k_ease", e2, 0.0f, 1.0f, "%.2f")) { k.ease_in = e2[0], k.ease_out = e2[1]; changed = true; }
      ImGui::PopItemWidth();
      if (changed) {
        char b[160];
        snprintf(b, sizeof b, "{\"cmd\":\"path_set_key\",\"index\":%d,\"t\":%.6f,\"ease_in\":%.4f,\"ease_out\":%.4f}", sel, k.t, k.ease_in, k.ease_out);
        run(b);
      }
      if (ImGui::Button("GO")) run("{\"cmd\":\"path_time\",\"t\":" + Fx(k.t) + "}");
      ImGui::SameLine();
      if (ImGui::Button("SET FROM CAMERA")) run("{\"cmd\":\"path_set_key\",\"index\":" + std::to_string(sel) + ",\"from_camera\":true}");
      ImGui::SameLine();
      if (ImGui::Button("DELETE")) run("{\"cmd\":\"path_delete\",\"index\":" + std::to_string(sel) + "}");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("FILE");
    ImGui::PushItemWidth(-1);
    ImGui::InputText("##p_name", st.name, sizeof st.name);
    ImGui::PopItemWidth();
    if (ImGui::Button("SAVE")) {
      if (run("{\"cmd\":\"path_save\",\"path\":\"" + jl::Esc(st.name) + "\"}")) st.msg = "saved";
    }
    ImGui::SameLine();
    if (ImGui::Button("LOAD")) {
      if (run("{\"cmd\":\"path_load\",\"path\":\"" + jl::Esc(st.name) + "\"}")) st.msg = "loaded";
    }
    ImGui::SameLine();
    if (ImGui::Button("CLEAR")) run("{\"cmd\":\"path_clear\"}");
    ImGui::TextDisabled("Bare name: %%LOCALAPPDATA%%\\wwe2k14\\viewer\\paths");

    ImGui::Separator();
    ImGui::TextUnformatted("RENDER (PNG sequence)");
    ImGui::PushItemWidth(-1);
    ImGui::InputText("##r_out", st.out, sizeof st.out);
    ImGui::PopItemWidth();
    ImGui::BeginDisabled(busy);
    ImGui::PushItemWidth(80);
    ImGui::InputInt("W", &st.w, 0);
    ImGui::SameLine();
    ImGui::InputInt("H", &st.h, 0);
    ImGui::InputInt("FPS", &st.fps, 0);
    ImGui::SameLine();
    ImGui::InputInt("SS", &st.ss, 0);
    ImGui::PopItemWidth();
    ImGui::PushItemWidth(-1);
    ImGui::TextUnformatted("MOTION BLUR (shutter) / SAMPLES");
    ImGui::SliderFloat("##r_blur", &st.blur, 0.0f, 1.0f, "%.2f");
    ImGui::SliderInt("##r_samp", &st.samples, 1, 32);
    ImGui::PopItemWidth();
    ImGui::Checkbox("SCENE CLOCK", &st.scene);
    ImGui::SameLine();
    ImGui::Checkbox("MP4 (ffmpeg)", &st.mp4);
    if (ImGui::Button("RENDER") && path.keys.size() >= 2) {
      char b[900];
      snprintf(b, sizeof b,
               "{\"cmd\":\"render\",\"path\":\"current\",\"out\":\"%s\",\"w\":%d,\"h\":%d,\"fps\":%d,\"ss\":%d,\"motion_blur\":%.3f,\"samples\":%d,\"mp4\":%s,\"scene\":%s}",
               jl::Esc(st.out).c_str(), st.w, st.h, st.fps, st.ss, st.blur, st.samples, st.mp4 ? "true" : "false", st.scene ? "true" : "false");
      if (run(b)) st.msg = "render started";
    }
    ImGui::EndDisabled();
    if (busy) {
      ImGui::SameLine();
      if (ImGui::Button("CANCEL")) run("{\"cmd\":\"render_cancel\"}");
      ImGui::ProgressBar(pu->frames > 0 ? (float)pu->frame / (float)pu->frames : 0.0f, ImVec2(-1, 0));
    }
    if (!pu->render_msg.empty() && pu->render_msg != "idle") {
      ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", pu->render_msg.c_str());
      ImGui::PopTextWrapPos();
    }
    if (!st.msg.empty()) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "%s", st.msg.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::TreePop();
  }
}

}  // namespace ui
}  // namespace view
