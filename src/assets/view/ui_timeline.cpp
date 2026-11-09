// Session 98: the entrance timeline bar (see ui_timeline.h).
#include "ui_timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace view {
namespace ui {
namespace {

constexpr ImU32 kRed = IM_COL32(214, 22, 30, 255);
constexpr double kSpeeds[5] = {0.25, 0.5, 1.0, 2.0, 4.0};
// Win32 virtual keys (this file has no windows.h)
constexpr unsigned kSpace = 0x20, kLeft = 0x25, kRight = 0x27, kComma = 0xBC, kPeriod = 0xBE, kMinus = 0xBD, kPlus = 0xBB;

void Seek(const std::function<void(const std::string&)>& send, double f, bool pause) {
  char b[160];
  std::snprintf(b, sizeof b, "{\"cmd\":\"entrance_time\",\"frame\":%.2f%s}", f, pause ? ",\"pause\":true" : "");
  send(b);
}
void Speed(const std::function<void(const std::string&)>& send, double sp) {
  char b[96];
  std::snprintf(b, sizeof b, "{\"cmd\":\"entrance_time\",\"speed\":%g}", sp);
  send(b);
}
int SpeedIndex(double sp) {
  int best = 2;
  for (int i = 0; i < 5; ++i)
    if (std::fabs(kSpeeds[i] - sp) < std::fabs(kSpeeds[best] - sp)) best = i;
  return best;
}
// mm:ss.ff, ff = the frame inside the second (the scene runs 60 per second)
std::string Clock(double frames) {
  const int f = std::max(0, (int)std::floor(frames + 1e-6));
  char b[48];
  std::snprintf(b, sizeof b, "%02d:%02d.%02d", f / 3600, (f / 60) % 60, f % 60);
  return b;
}
// Previous / next segment start (previous: back to this segment's start first, unless the playhead is within half a second of it).
double SegmentTarget(const EntranceView& ev, int dir) {
  if (ev.starts.empty()) return 0;
  const int n = (int)ev.starts.size();
  int cur = std::max(0, std::min(n - 1, ev.cue));
  if (dir < 0) {
    if (ev.t - ev.starts[(size_t)cur] > 30 || cur == 0) return ev.starts[(size_t)cur];
    return ev.starts[(size_t)cur - 1];
  }
  return cur + 1 < n ? ev.starts[(size_t)cur + 1] : ev.total;
}

}  // namespace

float TimelineHeight(float d) { return 122 * d; }

bool TimelineKey(unsigned vk, bool shift, const EntranceView& ev, TimelineState& st, const std::function<void(const std::string&)>& send) {
  if (!ev.active || ev.total <= 0) return false;
  const double base = st.key_t >= 0 ? st.key_t : ev.t;
  auto step = [&](double to) {
    to = std::max(0.0, std::min(ev.total, to));
    st.key_t = to;
    Seek(send, to, true);
  };
  switch (vk) {
    case kSpace: send(std::string("{\"cmd\":\"entrance_time\",\"play\":") + (ev.playing ? "false" : "true") + "}"); return true;
    case kLeft: step(std::floor(base + 1e-6) - (shift ? 10 : 1)); return true;
    case kRight: step(std::floor(base + 1e-6) + (shift ? 10 : 1)); return true;
    case kComma: case kPeriod: {
      EntranceView e = ev;
      e.t = base;
      const double to = SegmentTarget(e, vk == kComma ? -1 : 1);
      st.key_t = std::max(0.0, std::min(ev.total, to));
      Seek(send, st.key_t, false);
      return true;
    }
    case kMinus: case kPlus: {
      const int i = std::max(0, std::min(4, SpeedIndex(ev.speed) + (vk == kPlus ? 1 : -1)));
      Speed(send, kSpeeds[i]);
      return true;
    }
  }
  return false;
}

void DrawTimeline(const EntranceView& ev, TimelineState& st, const TimelineCtx& cx) {
  if (!ev.active || ev.total <= 0 || ev.starts.empty()) return;
  const float d = cx.d, H = TimelineHeight(d), W = std::max(200.0f * d, cx.disp.x - cx.right_margin);
  if (!cx.pending) st.key_t = -1;
  // loop: the entrance ran to its end while it was playing -> start over
  if (st.loop && st.prev_playing && !ev.playing && ev.t >= ev.total - 0.5) Seek(cx.send, 0, false), cx.send("{\"cmd\":\"entrance_time\",\"play\":true}");
  st.prev_playing = ev.playing;

  ImGui::SetNextWindowPos(ImVec2(0, cx.disp.y), ImGuiCond_Always, ImVec2(0, 1));
  ImGui::SetNextWindowSize(ImVec2(W, H));
  ImGui::SetNextWindowBgAlpha(0.94f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##timeline", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetWindowPos();
  dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), IM_COL32(19, 20, 24, 255));
  dl->AddRectFilled(o, ImVec2(o.x + W, o.y + 3 * d), kRed);  // the red top edge

  const float pad = 16 * d, bh = 28 * d, px = 14 * d;
  auto button = [&](const char* id, const char* label, float x, float w, bool on) {
    const ImVec2 a(o.x + x, o.y + 10 * d), b(a.x + w, a.y + bh);
    ImGui::SetCursorScreenPos(a);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(w, bh));
    const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    dl->AddRectFilled(a, b, on ? kRed : held ? IM_COL32(120, 16, 22, 255) : hov ? IM_COL32(140, 20, 28, 255) : IM_COL32(44, 46, 56, 255), 2 * d);
    const ImVec2 ts = cx.size(px, label);
    cx.text(dl, ImVec2(a.x + (w - ts.x) / 2, a.y + (bh - ts.y) / 2), px, IM_COL32_WHITE, label);
    return hit;
  };
  auto send = cx.send;
  float x = pad;
  const float gap = 6 * d;
  if (button("##prev", "PREV", x, 54 * d, false)) Seek(send, SegmentTarget(ev, -1), false);
  x += 54 * d + gap;
  if (button("##b1", "-1F", x, 46 * d, false)) Seek(send, std::max(0.0, std::floor(ev.t + 1e-6) - 1), true);
  x += 46 * d + gap;
  if (button("##play", ev.playing ? "PAUSE" : "PLAY", x, 80 * d, ev.playing)) send(std::string("{\"cmd\":\"entrance_time\",\"play\":") + (ev.playing ? "false" : "true") + "}");
  x += 80 * d + gap;
  if (button("##f1", "+1F", x, 46 * d, false)) Seek(send, std::min(ev.total, std::floor(ev.t + 1e-6) + 1), true);
  x += 46 * d + gap;
  if (button("##next", "NEXT", x, 54 * d, false)) Seek(send, SegmentTarget(ev, 1), false);
  x += 54 * d + gap * 2;
  if (button("##loop", "LOOP", x, 56 * d, st.loop)) st.loop = !st.loop;
  x += 56 * d + gap * 3;
  const char* const sl[5] = {"0.25x", "0.5x", "1x", "2x", "4x"};
  const int cur_sp = std::fabs(ev.speed - kSpeeds[SpeedIndex(ev.speed)]) < 1e-6 ? SpeedIndex(ev.speed) : -1;
  for (int i = 0; i < 5; ++i) {
    char id[16];
    std::snprintf(id, sizeof id, "##sp%d", i);
    const float w = (i == 0 ? 56 : 46) * d;
    if (button(id, sl[i], x, w, cur_sp == i)) Speed(send, kSpeeds[i]);
    x += w + gap;
  }
  {  // the camera: ENTRANCE CAM <-> FREE (right-click / F in the scene hand over to FREE by themselves)
    const float w = 138 * d;
    if (button("##cam", ev.cam_entrance ? "ENTRANCE CAM" : "FREE CAM", W - pad - w, w, ev.cam_entrance))
      send(std::string("{\"cmd\":\"entrance_time\",\"camera\":\"") + (ev.cam_entrance ? "free" : "entrance") + "\"}");
  }

  // readout row
  const int ci = std::max(0, std::min((int)ev.labels.size() - 1, ev.cue));
  {
    const float ry = o.y + 10 * d + bh + 6 * d, rpx = 15 * d;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s   FRAME %d / %d   TOTAL %s", Clock(ev.t).c_str(), (int)std::floor(ev.t + 1e-6), (int)std::floor(ev.total + 1e-6), Clock(ev.total).c_str());
    cx.text(dl, ImVec2(o.x + pad, ry), rpx, IM_COL32_WHITE, buf);
    if (!ev.labels.empty()) {
      char nb[160];
      std::snprintf(nb, sizeof nb, "NOW  %s   CUE %d", ev.labels[(size_t)ci].c_str(), ci < (int)ev.cue_ids.size() ? ev.cue_ids[(size_t)ci] : ci);
      const ImVec2 ts = cx.size(rpx, nb);
      cx.text(dl, ImVec2(o.x + W - pad - ts.x, ry), rpx, IM_COL32(255, 150, 154, 255), nb);
    }
  }

  // the track
  const float ty = o.y + 10 * d + bh + 6 * d + 22 * d, th = 46 * d, tx0 = o.x + pad, tw = W - 2 * pad;
  const double total = ev.total;
  auto xs = [&](double f) { return tx0 + float(std::max(0.0, std::min(total, f)) / total) * tw; };
  const int hover_seg = [&] {
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (m.x < tx0 || m.x > tx0 + tw || m.y < ty || m.y > ty + th) return -1;
    return 0;
  }();
  for (size_t i = 0; i < ev.starts.size(); ++i) {
    const float a = xs(ev.starts[i]), b = xs(ev.starts[i] + ev.lens[i]);
    const bool curseg = (int)i == ev.cue;
    dl->AddRectFilled(ImVec2(a, ty), ImVec2(b, ty + th), curseg ? IM_COL32(112, 22, 30, 255) : (i & 1) ? IM_COL32(52, 55, 68, 255) : IM_COL32(37, 39, 49, 255));
    dl->AddRect(ImVec2(a, ty), ImVec2(b, ty + th), IM_COL32(0, 0, 0, 140));
    if (curseg) dl->AddRectFilled(ImVec2(a, ty), ImVec2(b, ty + 3 * d), kRed);
    const std::string& lab = i < ev.labels.size() ? ev.labels[i] : ev.names[i];
    const float room = b - a - 8 * d;
    const float lp = 15 * d;
    if (room > 12 * d) {
      std::string shown = lab;
      while (!shown.empty() && cx.size(lp, shown.c_str()).x > room) shown.pop_back();  // (clipped, never overflowing into the next segment)
      if (!shown.empty()) cx.text(dl, ImVec2(a + 5 * d, ty + 6 * d), lp, IM_COL32_WHITE, shown.c_str());
      char cn[48];
      std::snprintf(cn, sizeof cn, "CUE %d  %.1fs", i < ev.cue_ids.size() ? ev.cue_ids[i] : (int)i, ev.lens[i] / 60.0);
      std::string cs = cn;
      while (!cs.empty() && cx.size(10.5f * d, cs.c_str()).x > room) cs.pop_back();
      if (!cs.empty()) cx.text(dl, ImVec2(a + 5 * d, ty + th - 17 * d), 10.5f * d, IM_COL32(200, 200, 208, 220), cs.c_str());
    }
  }
  {  // the part already played: a faint red wash; second ticks along the bottom edge
    dl->AddRectFilled(ImVec2(tx0, ty + th - 4 * d), ImVec2(xs(ev.t), ty + th), IM_COL32(214, 22, 30, 200));
    const double step = total / 60.0 > 90 ? 600.0 : total / 60.0 > 30 ? 300.0 : 60.0;  // frames between ticks
    for (double f = 0; f <= total; f += step) dl->AddLine(ImVec2(xs(f), ty + th - 7 * d), ImVec2(xs(f), ty + th), IM_COL32(255, 255, 255, 110));
  }
  // the playhead: click or drag anywhere on the track
  ImGui::SetCursorScreenPos(ImVec2(tx0, ty - 6 * d));
  ImGui::InvisibleButton("##track", ImVec2(tw, th + 12 * d));
  if (ImGui::IsItemActivated()) st.scrubbing = true, st.resume = ev.playing, st.last_sent = -1;
  if (ImGui::IsItemActive()) {
    const double f = std::max(0.0, std::min(total, double((ImGui::GetIO().MousePos.x - tx0) / tw) * total));
    if (std::fabs(f - st.last_sent) > 0.01) Seek(send, f, true), st.last_sent = f;
  }
  if (ImGui::IsItemDeactivated()) {
    if (st.resume) send("{\"cmd\":\"entrance_time\",\"play\":true}");
    st.scrubbing = false;
  }
  // (Session 98 phone fixes: no hover tooltip from a finger - it stayed up over the track after a tap or a scrub)
  if (hover_seg >= 0 && ImGui::IsItemHovered() && !st.scrubbing && ImGui::GetIO().MouseSource != ImGuiMouseSource_TouchScreen) {
    const double f = double((ImGui::GetIO().MousePos.x - tx0) / tw) * total;
    int si = 0;
    for (size_t i = 0; i < ev.starts.size(); ++i)
      if (f >= ev.starts[i]) si = (int)i;
    if (si < (int)ev.tips.size()) ImGui::SetTooltip("%s\n%s", si < (int)ev.labels.size() ? ev.labels[(size_t)si].c_str() : "", ev.tips[(size_t)si].c_str());
  }
  {
    const float hx = xs(st.scrubbing && st.last_sent >= 0 ? st.last_sent : ev.t);
    dl->AddLine(ImVec2(hx, ty - 4 * d), ImVec2(hx, ty + th + 2 * d), IM_COL32_WHITE, 2 * d);
    dl->AddTriangleFilled(ImVec2(hx - 6 * d, ty - 9 * d), ImVec2(hx + 6 * d, ty - 9 * d), ImVec2(hx, ty - 1 * d), kRed);
    dl->AddTriangle(ImVec2(hx - 6 * d, ty - 9 * d), ImVec2(hx + 6 * d, ty - 9 * d), ImVec2(hx, ty - 1 * d), IM_COL32_WHITE, 1.2f * d);
  }
  ImGui::End();
}

}  // namespace ui
}  // namespace view
