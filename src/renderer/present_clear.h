// S102 G1 lever 2 (present_noclear): the swapchain's pass cleared the whole panel every frame (swapchain.cpp loadOp CLEAR). The draw covers the
// viewport, so only the bars around it need a clear: nothing at all when the viewport covers the panel. Pure arithmetic (no Vulkan), unit tested
// by tools\g1\low_levers_test.cpp (planted twins: tools\g1\g1_check.py).
#pragma once
#include <algorithm>
#include <cstdint>

namespace rdr {

struct PresentBar { int32_t x, y; uint32_t w, h; };

// The rectangles of the panel (fb_w x fb_h) that the viewport (vx, vy, vw, vh; whole pixels) does not cover: top, bottom, left, right bars.
// Returns the count (0..4) written to out[4]. A viewport that is empty or outside the panel leaves the whole panel to clear (one rect).
inline int PresentBars(int32_t fb_w, int32_t fb_h, int32_t vx, int32_t vy, int32_t vw, int32_t vh, PresentBar out[4]) {
  if (vw <= 0 || vh <= 0 || vx >= fb_w || vy >= fb_h || vx + vw <= 0 || vy + vh <= 0) {
    out[0] = {0, 0, uint32_t(fb_w), uint32_t(fb_h)};
    return 1;
  }
  const int32_t x0 = std::max(vx, 0), y0 = std::max(vy, 0), x1 = std::min(vx + vw, fb_w), y1 = std::min(vy + vh, fb_h);
  int n = 0;
  if (y0 > 0) out[n++] = {0, 0, uint32_t(fb_w), uint32_t(y0)};                      // top
  if (y1 < fb_h) out[n++] = {0, y1, uint32_t(fb_w), uint32_t(fb_h - y1)};          // bottom
  if (x0 > 0) out[n++] = {0, y0, uint32_t(x0), uint32_t(y1 - y0)};                 // left (between the bars)
  if (x1 < fb_w) out[n++] = {x1, y0, uint32_t(fb_w - x1), uint32_t(y1 - y0)};      // right
  return n;
}

}  // namespace rdr
