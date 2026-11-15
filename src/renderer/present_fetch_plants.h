// S102 R24-A (wwe2k14_rdr_present_fetch, docs\renderer\PRESENT_COPY.md): the offline tools' display curve, overlay and census line. TEST ONLY: rplay and fview call PresentFetchPlants() once after the
// renderer is made; with no flag it does nothing. The game never includes this header.
//   --display-curve-s100      the game's display curve as the S100 runs saw it (LIVE_ADAPTER.md "The display table": 16 -> 6, 32 -> 16, 64 -> 48, 128 -> 115, 192 -> 185), made by straight lines between those five
//                             points and (0, 0) / (255, 255). Captures carry no table and the tools start from the identity, so every proof of the present runs WITH a real curve, as the live game does.
//   --display-curve-file F    1024 bytes, 256 x RGBA8 (the table the reader hands the renderer, reader.h OnGammaRamp), e.g. dumped from a live run.
//   --overlay-plant           an overlay of one opaque rectangle (the present draws it after the picture).
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "output_switches.h"
#include "renderer.h"

namespace rdr {

inline std::string g_display_curve_file;  // --display-curve-file (set while the arguments are read, before any thread)

inline std::string PresentFetchLine(const char* tag) {
  const auto get = [](PresentFetchSlot s) { return static_cast<unsigned long long>(g_present_fetch_count[s].load(std::memory_order_relaxed)); };
  char buf[300];
  std::snprintf(buf, sizeof buf, "%s[present_fetch] switch %d, frames %llu, taken %llu, declined fit %llu sgsr_dlss %llu cap %llu", tag, g_present_fetch.load() ? 1 : 0, get(kPcFrames), get(kPcTaken), get(kPcDeclFit),
                get(kPcDeclSgsrDlss), get(kPcDeclCap));
  return buf;
}

inline void PresentFetchPlants(Renderer& renderer) {
  uint8_t curve[256 * 4];
  bool have = false;
  if (!g_display_curve_file.empty()) {
    if (std::FILE* f = std::fopen(g_display_curve_file.c_str(), "rb")) {
      have = std::fread(curve, 1, sizeof curve, f) == sizeof curve;
      std::fclose(f);
    }
    if (!have) std::fprintf(stderr, "[present_fetch] --display-curve-file %s: could not read 1024 bytes\n", g_display_curve_file.c_str());
  } else if (g_display_curve_s100.load(std::memory_order_relaxed)) {
    static const int xs[] = {0, 16, 32, 64, 128, 192, 255}, ys[] = {0, 6, 16, 48, 115, 185, 255};
    for (int i = 0; i < 256; ++i) {
      int seg = 0;
      while (seg < 5 && i > xs[seg + 1]) ++seg;
      const float t = float(i - xs[seg]) / float(xs[seg + 1] - xs[seg]);
      const uint8_t v = uint8_t(float(ys[seg]) + t * float(ys[seg + 1] - ys[seg]) + 0.5f);
      curve[i * 4] = curve[i * 4 + 1] = curve[i * 4 + 2] = v, curve[i * 4 + 3] = 255;
    }
    have = true;
  }
  if (have) renderer.SetDisplayCurve(curve);
  if (g_overlay_plant.load(std::memory_order_relaxed)) {
    const uint8_t white[4] = {255, 255, 255, 255};
    renderer.SetOverlayTexture(0, 1, 1, white);
    auto list = std::make_shared<OverlayList>();
    list->width = 100, list->height = 100;
    const uint32_t red = 0xFF0000FFu;  // RGBA8, R in the low byte
    list->vertices = {{10, 10, 0, 0, red}, {60, 10, 1, 0, red}, {60, 40, 1, 1, red}, {10, 40, 0, 1, red}};
    list->indices = {0, 1, 2, 0, 2, 3};
    OverlayDraw draw;
    draw.texture = 0, draw.first_index = 0, draw.count = 6;
    list->draws.push_back(draw);
    renderer.SetOverlay(list);
  }
}

}  // namespace rdr
