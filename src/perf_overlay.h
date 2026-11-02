// wwe2k14 - the performance overlay (Session 80; Jarrod: "a little fps/frametime/temperature graph ... we'll put the
// toggle in the logging settings").
//
// --wwe2k14_perf_overlay (default 0, off; the phone launcher: LOGS AND UPDATES -> PERFORMANCE OVERLAY) draws in the
// top-left corner over the game one line - the frame rate, the frame time and (Android) the phone's temperature, e.g.
// "60 FPS  16.6 ms  35.0 C". 1 = light (Jarrod: "more want just the numbers, and no box either, don't want it to be too
// noticeable"): the numbers alone, small, with a soft shadow; 2 = verbose: in a box with the last 2 seconds of frame
// times as a small graph, lines at 16.7 ms (60 fps) and 33.3 ms (30 fps). The frame times are the game's own (its
// frames, FrameStats::OnSwap - as the frame log's
// frame_ms). The temperature is the battery's, from Android's battery broadcast (GameActivity.java ->
// nativeBatteryTemperature, tenths of a degree): the one every app may read; none on the PC.
// An ImGui overlay, as the check banner (src\check_prompt.h): it is not in screenshots or clips (they capture the
// game's own picture).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <functional>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#if defined(__ANDROID__)
#include <jni.h>
#endif

REXCVAR_DEFINE_INT32(wwe2k14_perf_overlay, 0, "wwe2k14",
                     "Session 80: the frame rate, frame time and (Android) the phone's temperature in the top-left "
                     "corner: 1 = light (the numbers only), 2 = verbose (in a box, with a graph of the last 2 seconds "
                     "of frame times). The phone launcher's LOGS AND UPDATES -> PERFORMANCE OVERLAY. Read at start");

namespace wwe2k14::perf_overlay {

inline constexpr uint32_t kHistory = 120;  // frames in the graph (2 s at 60 fps)

struct Data {
  std::array<std::atomic<float>, kHistory> ms{};  // a ring of the game's frame times
  std::atomic<uint32_t> next{0};                   // frames pushed so far
  std::atomic<int> battery_tenths{INT_MIN};        // Android: the battery's temperature, tenths of a degree C
};

inline Data& D() {
  static Data d;
  return d;
}

// The game's frame thread (FrameStats::OnSwap), every frame after the first.
inline void PushFrame(double ms) {
  if (REXCVAR_GET(wwe2k14_perf_overlay) <= 0) return;
  Data& d = D();
  const uint32_t i = d.next.load(std::memory_order_relaxed);
  d.ms[i % kHistory].store(float(ms), std::memory_order_relaxed);
  d.next.store(i + 1, std::memory_order_release);
}

class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    Data& d = D();
    const uint32_t count = d.next.load(std::memory_order_acquire);
    const uint32_t n = std::min(count, kHistory);
    // The graph's frames, oldest first; the frame rate over the last second, the frame time over the last 10 frames.
    std::array<float, kHistory> graph{};
    for (uint32_t k = 0; k < n; ++k) graph[k] = d.ms[(count - n + k) % kHistory].load(std::memory_order_relaxed);
    double second = 0.0, recent = 0.0;
    uint32_t in_second = 0, in_recent = 0;
    for (uint32_t k = n; k-- > 0;) {
      if (second < 1000.0) second += graph[k], ++in_second;
      if (in_recent < 10) recent += graph[k], ++in_recent;
    }
    const double fps = second > 0.0 ? 1000.0 * in_second / second : 0.0;
    const double frame_ms = in_recent ? recent / in_recent : 0.0;
    char line[64];
    const int tenths = d.battery_tenths.load(std::memory_order_relaxed);
#if defined(__ANDROID__)
    if (tenths != INT_MIN) {
      std::snprintf(line, sizeof(line), "%.0f FPS  %.1f ms  %.1f C", fps, frame_ms, tenths / 10.0);
    } else {
      std::snprintf(line, sizeof(line), "%.0f FPS  %.1f ms  --.- C", fps, frame_ms);
    }
#else
    (void)tenths;
    std::snprintf(line, sizeof(line), "%.0f FPS  %.1f ms", fps, frame_ms);
#endif
    // Sized to the screen: the phone's 1440-row screen gets text as large as a 720p window's at its size.
    const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    const bool verbose = REXCVAR_GET(wwe2k14_perf_overlay) >= 2;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    // Green as a game overlay; the frame time yellow over 20 ms, red over 33 ms.
    const ImVec4 color = frame_ms > 33.4 ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f)
                         : frame_ms > 20.0 ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                           : ImVec4(0.35f, 1.0f, 0.35f, 1.0f);
    if (!verbose) {
      // Light: the numbers alone - no box, smaller, a little see-through, a soft shadow to read them over a bright
      // picture.
      ImGui::SetNextWindowPos(ImVec2(6.0f * scale, 4.0f * scale), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      if (ImGui::Begin("##wwe2k14_perf", nullptr, flags)) {
        ImGui::SetWindowFontScale(0.9f * scale);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float off = std::max(1.0f, scale);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + off, at.y + off), IM_COL32(0, 0, 0, 150), line);
        ImGui::TextColored(ImVec4(color.x, color.y, color.z, 0.85f), "%s", line);
      }
      ImGui::End();
      ImGui::PopStyleVar();
      return;
    }
    ImGui::SetNextWindowPos(ImVec2(8.0f * scale, 8.0f * scale), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##wwe2k14_perf", nullptr, flags)) {
      ImGui::SetWindowFontScale(1.1f * scale);
      ImGui::TextColored(color, "%s", line);
      // The graph: 0..50 ms, newest on the right; lines at 60 and 30 fps.
      const ImVec2 size(ImGui::CalcTextSize(line).x, 34.0f * scale);
      const ImVec2 origin = ImGui::GetCursorScreenPos();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float max_ms = 50.0f;
      const auto y_of = [&](float ms) { return origin.y + size.y * (1.0f - std::min(ms, max_ms) / max_ms); };
      dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 90));
      dl->AddLine(ImVec2(origin.x, y_of(16.7f)), ImVec2(origin.x + size.x, y_of(16.7f)), IM_COL32(90, 255, 90, 70));
      dl->AddLine(ImVec2(origin.x, y_of(33.3f)), ImVec2(origin.x + size.x, y_of(33.3f)), IM_COL32(255, 100, 90, 70));
      if (n >= 2) {
        const float step = size.x / float(kHistory - 1);
        const float x0 = origin.x + size.x - step * float(n - 1);
        for (uint32_t k = 1; k < n; ++k) {
          const ImU32 c = graph[k] > 33.4f   ? IM_COL32(255, 100, 90, 255)
                          : graph[k] > 20.0f ? IM_COL32(255, 215, 80, 255)
                                             : IM_COL32(90, 255, 90, 255);
          dl->AddLine(ImVec2(x0 + step * float(k - 1), y_of(graph[k - 1])), ImVec2(x0 + step * float(k), y_of(graph[k])),
                      c, 1.5f * scale);
        }
      }
      ImGui::Dummy(size);
    }
    ImGui::End();
  }
};

// The app's UI setup (OnPostSetup, beside check::Install): the overlay for the run when the setting is on.
inline void Install(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_ui) {
  if (REXCVAR_GET(wwe2k14_perf_overlay) <= 0 || !drawer || !post_ui) return;
  post_ui([drawer] {
    static Overlay* overlay = nullptr;  // for the whole run (the drawer outlives the game)
    if (!overlay) overlay = new Overlay(drawer);
  });
}

}  // namespace wwe2k14::perf_overlay

#if defined(__ANDROID__)
// GameActivity.java: Android's battery broadcast (EXTRA_TEMPERATURE, tenths of a degree C), at start and on changes.
extern "C" JNIEXPORT void JNICALL Java_io_github_jarrodfeng_wwe2k14_GameActivity_nativeBatteryTemperature(JNIEnv*, jclass,
                                                                                                         jint tenths) {
  wwe2k14::perf_overlay::D().battery_tenths.store(int(tenths), std::memory_order_relaxed);
}
#endif
