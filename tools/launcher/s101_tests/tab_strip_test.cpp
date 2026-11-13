// Session 101 lane 285: layout test for the launcher's tab header rows (src\launcher\tab_strip.h), no ImGui, no game.
// Build + run: clang++ -std=c++20 -I src/launcher tools/launcher/s101_tests/tab_strip_test.cpp -o out/tab_strip_test.exe && out/tab_strip_test.exe
// Models the MODS row exactly as ModsPage lays it out: [MODS heading][28 d][strip][10 d + 16 d guard][IMPORTER 260 d][10 d][VIEWER 210 d]
// at the right edge. Header widths: text width ~ 0.62 * 21 d per glyph + 36 d padding (the real widths come from the font; the
// sweep covers 0.45..0.8 of em per glyph). Window widths 700..3840 px, DPI scale 100..200 %. The left red page panel is modelled as
// 18% of the window (min 220 px).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tab_strip.h"

using namespace launcher::tabs;

static int fails = 0, checks = 0, overflow = 0, old_overlap = 0, toggles = 0, toggle_same = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    ++checks;                                          \
    if (!(c)) {                                        \
      if (++fails < 30) { std::printf("FAIL %s: ", #c); std::printf(__VA_ARGS__); std::printf("\n"); } \
    }                                                  \
  } while (0)

int main() {
  const char* names[5] = {"TRONS", "TITLES", "LOGOS", "MUSIC", "SUPERSTARS"};
  const char* era[5] = {"HULKAMANIA", "NEW GENERATION", "ATTITUDE", "RUTHLESS AGGRESSION", "UNIVERSE"};
  for (int dpi = 100; dpi <= 200; dpi += 25)
    for (float em : {0.45f, 0.62f, 0.8f})
      for (int win = 700; win <= 3840; win += 20) {
        const float d = dpi / 100.f * 1.0f, px = 21 * d;
        const float panel = std::max(220.f, win * 0.18f), content_max = win - panel - 40 * d;
        // MODS heading ~ 4 glyphs at 1.6 * px
        const float heading = 4 * 1.6f * px * em + 28 * d;
        const float viewer_w = 210 * d, imp_w = 260 * d, gap = 2 * d, arrow_w = 26 * d;
        const float buttons_left = content_max - viewer_w - imp_w - 10 * d;  // the IMPORTER button's left edge
        const float avail = std::max(buttons_left - heading - 16 * d, 4 * arrow_w);
        for (int rows = 0; rows < 2; ++rows) {
          const char* const* nm = rows ? era : names;
          std::vector<float> w(5);
          for (int i = 0; i < 5; ++i) w[i] = std::strlen(nm[i]) * px * em + 36 * d;
          const float row_avail = rows ? std::max(content_max, 4 * arrow_w) : avail;
          // every header reachable and visible when selected, whatever the window's state
          if (Span(w.data(), 0, 5, gap) > row_avail) ++overflow;
          if (!rows && heading + Span(w.data(), 0, 5, gap) > buttons_left) ++old_overlap;
          int first = 0, prev_sel = -1;
          bool seen[5] = {};
          for (int sel = 0; sel < 5; ++sel) {
            Layout L = Fit(w.data(), 5, gap, row_avail, arrow_w, sel, first, sel != prev_sel);
            prev_sel = sel;
            CHECK(L.used <= row_avail + 0.01f, "dpi %d em %.2f win %d row %d used %.1f > avail %.1f", dpi, em, win, rows, L.used, row_avail);
            CHECK(L.last > L.first, "empty window");
            CHECK(sel >= L.first && sel < L.last, "selected %d not visible [%d,%d) win %d dpi %d", sel, L.first, L.last, win, dpi);
            if (!rows) {  // the strip's right edge vs the buttons' left edge
              CHECK(heading + L.used <= buttons_left - 16 * d + 0.01f || avail == 4 * arrow_w, "strip runs under the buttons: win %d dpi %d", win, dpi);
            }
            // arrows alone reach all: walk right then left
            for (int i = L.first; i < L.last; ++i) seen[i] = true;
          }
          // arrow-only reachability from the first position
          int f = 0;
          Layout L = Fit(w.data(), 5, gap, row_avail, arrow_w, 0, f, true);
          bool reach[5] = {};
          for (int step = 0; step < 12; ++step) {
            for (int i = L.first; i < L.last; ++i) reach[i] = true;
            if (!L.arrows) break;
            f = std::min(f + 1, 4);  // right arrow
            L = Fit(w.data(), 5, gap, row_avail, arrow_w, 0, f, false);
          }
          for (int i = 0; i < 5; ++i) CHECK(reach[i], "header %d not reachable by the right arrow: win %d dpi %d row %d", i, win, dpi, rows);
          if (!rows) {  // the USE MY ... switch: beside the strip only if it fits between strip and buttons, else its own row below
            const float x_after = heading + L.used + 24 * d, right = buttons_left - 16 * d;
            const Toggle T = ToggleSlot(right - x_after, content_max, d);
            ++toggles;
            if (T.same_line) {
              ++toggle_same;
              CHECK(right - T.w >= x_after - 0.01f, "toggle overlaps the headers: win %d dpi %d", win, dpi);
              CHECK(right <= buttons_left + 0.01f, "toggle reaches the buttons: win %d dpi %d", win, dpi);
            } else {
              CHECK(T.w <= content_max + 0.01f && T.w > 0, "toggle row wider than the window: win %d dpi %d w %.1f", win, dpi, T.w);
            }
          }
          // and back to the start with the left arrow
          for (int step = 0; step < 12 && L.arrows; ++step) {
            f = std::max(f - 1, 0);
            L = Fit(w.data(), 5, gap, row_avail, arrow_w, 4, f, false);
          }
          CHECK(L.first == 0 || !L.arrows, "left arrow does not return to the first header");
        }
      }
  // the old layout, for the record: the fixed layout overlapped at these widths
  {
    const float d = 1, px = 21;
    std::vector<float> w(5);
    for (int i = 0; i < 5; ++i) w[i] = std::strlen(names[i]) * px * 0.62f + 36;
    int first = 0;
    Layout L = Fit(w.data(), 5, 2, 400, 26, 3, first, true);
    CHECK(L.arrows && L.first <= 3 && 3 < L.last, "MUSIC (tab 3) stays visible in a 400 px strip: [%d,%d)", L.first, L.last);
    (void)d;
  }
  std::printf("%d checks, %d failures; overflow cases %d (old fixed layout ran under the buttons in %d)\n", checks, fails, overflow, old_overlap);
  return fails ? 1 : 0;
}
