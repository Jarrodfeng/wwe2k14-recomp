// Session 101 (lane 285, Jarrod 2026-11-13: "the trons, titles, music headers can end up going behind the superstar import and
// arena viewer buttons ... this should wrap properly, perhaps with arrows"): the layout of a row of game-style tab headers that
// must fit in a given width. Pure math (no ImGui), so tools\launcher\s101_tests\tab_strip_test.cpp can sweep window widths.
//
// All headers fit -> all are shown, no arrows. Otherwise a left and a right arrow (each arrow_w + gap wide, both reserved so the strip
// does not jump) frame a window of headers [first, last); the window always holds at least one header and never grows past `avail`.
// The caller keeps `first` between frames; arrows move it by one. `ensure` (true on the frame the selection changed) slides the window
// until the selected header is inside it. The caller floors `avail` at a few arrow widths (a window that narrow is unusable anyway).
#pragma once

#include <algorithm>

namespace launcher::tabs {

struct Layout {
  int first = 0, last = 0;  // headers [first, last) are drawn
  bool arrows = false;      // overflow: draw the arrow buttons (one is disabled at an end of the list)
  float used = 0;           // total width of the strip (arrows included); always <= avail
  float lone_w = 0;         // > 0: even one header does not fit; it is drawn this wide (its text clipped)
};

inline float Span(const float* w, int a, int b, float gap) {  // width of headers [a, b) with gaps between
  float t = 0;
  for (int i = a; i < b; ++i) t += w[i] + (i > a ? gap : 0);
  return t;
}

// w[n]: header widths. avail: the width the whole strip may take. first: in/out, the window's first header.
inline Layout Fit(const float* w, int n, float gap, float avail, float arrow_w, int sel, int& first, bool ensure) {
  Layout L;
  if (n <= 0) return L;
  avail = std::max(avail, 0.0f);
  if (Span(w, 0, n, gap) <= avail) {
    first = 0, L.first = 0, L.last = n, L.used = Span(w, 0, n, gap);
    return L;
  }
  L.arrows = true;
  const float room = std::max(avail - 2 * (arrow_w + gap), 0.0f);  // for the headers
  first = std::clamp(first, 0, n - 1);
  sel = std::clamp(sel, 0, n - 1);
  if (ensure && sel < first) first = sel;
  auto lastFor = [&](int f) {  // greedy: as many headers as fit, at least one
    int l = f + 1;
    while (l < n && Span(w, f, l + 1, gap) <= room) ++l;
    return l;
  };
  if (ensure)
    while (first < sel && lastFor(first) <= sel) ++first;
  // the end of the list never leaves room that earlier headers could use
  while (first > 0 && lastFor(first) == n && Span(w, first - 1, n, gap) <= room) --first;
  L.first = first, L.last = lastFor(first);
  float body = Span(w, L.first, L.last, gap);
  if (body > room) body = L.lone_w = room;
  L.used = std::min(body + 2 * (arrow_w + gap), avail);
  return L;
}

// The USE MY ... switch beside the headers. room: width between the end of the strip and the left edge of the buttons; row_w: the width
// of the whole content row. Enough room -> same row, right-aligned against the buttons' left edge, at most 470 d wide. Otherwise its own
// row BELOW the buttons (the caller moves the cursor under them), as wide as the row allows: never beside or under the buttons.
struct Toggle {
  bool same_line = false;
  float w = 0;
};
inline Toggle ToggleSlot(float room, float row_w, float d) {
  Toggle t;
  if (room >= 330 * d) t.same_line = true, t.w = std::min(470 * d, room);
  else t.w = std::min(470 * d, std::max(row_w, 0.0f));
  return t;
}

}  // namespace launcher::tabs
