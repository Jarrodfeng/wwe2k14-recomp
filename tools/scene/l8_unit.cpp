// S103 L8: the match page's fade, the consumer's own code (assets::stage_pools::FadeAlpha255 / FadeFrom) on raw RGBA8 pages. Compile only, no game.
//   l8_unit.exe                              self-test (exit 1 on a failure)
//   l8_unit.exe BASE.raw MATCH.raw A255 OUT.raw   writes FadeFrom(base, match, A255) (BASE / MATCH: w*h*4 bytes, w = h = 512)
//   l8_unit.exe --alpha T                    prints FadeAlpha255(T)
// tools\scene\l8_check.py builds BASE / MATCH from the player's own pac and compares OUT with the game's page pixels of a stream.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../src/assets/stage_pools.h"

namespace sp = assets::stage_pools;

static int g_fail = 0;
static void Expect(bool ok, const char* what) {
  if (!ok) std::printf("FAIL  %s\n", what), ++g_fail;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--alpha") == 0) {
    std::printf("%d\n", sp::FadeAlpha255(std::strtoull(argv[2], nullptr, 10)));
    return 0;
  }
  if (argc == 5) {
    sp::Cache c;
    c.ok = true, c.w = c.h = 512;
    const size_t n = size_t(512) * 512 * 4;
    c.base.resize(n), c.match.resize(n);
    FILE* f = std::fopen(argv[1], "rb");
    if (!f || std::fread(c.base.data(), 1, n, f) != n) return std::printf("cannot read %s\n", argv[1]), 2;
    std::fclose(f);
    f = std::fopen(argv[2], "rb");
    if (!f || std::fread(c.match.data(), 1, n, f) != n) return std::printf("cannot read %s\n", argv[2]), 2;
    std::fclose(f);
    std::vector<uint8_t> out;
    sp::FadeFrom(c, std::atoi(argv[3]), out);
    f = std::fopen(argv[4], "wb");
    if (!f) return 2;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return 0;
  }
  // the page clock (listing of sub_825BAA28: rem = t mod 600, alpha = int(255 * rem / 600); page 01 alone from t = 600, nothing drawn from 1200: the target holds page 01)
  Expect(sp::FadeAlpha255(0) == 0, "alpha(0) = 0");
  Expect(sp::FadeAlpha255(1) == 0, "alpha(1) = 0 (int(0.425))");
  Expect(sp::FadeAlpha255(2) == 0, "alpha(2) = 0 (int(0.85))");
  Expect(sp::FadeAlpha255(3) == 1, "alpha(3) = 1");
  Expect(sp::FadeAlpha255(304) == 129, "alpha(304) = 129 (the stream's first fade draw: 0.5059 * 255)");
  Expect(sp::FadeAlpha255(306) == 130, "alpha(306) = 130 (the stream's third frame: 0.5098 * 255)");
  Expect(sp::FadeAlpha255(599) == 254, "alpha(599) = 254");
  Expect(sp::FadeAlpha255(600) == 255 && sp::FadeAlpha255(5000) == 255, "alpha(>= 600) = 255");
  sp::Cache c;
  c.ok = true, c.w = 1, c.h = 1;
  c.base = {10, 100, 200, 255};
  c.match = {110, 0, 255, 255};
  std::vector<uint8_t> o;
  sp::FadeFrom(c, 0, o);
  Expect(o == c.base, "a = 0 is the base page");
  sp::FadeFrom(c, 255, o);
  Expect(o == c.match, "a = 255 is the match page");
  sp::FadeFrom(c, 128, o);  // 10 + 100 * 128 / 255 = 60.2, 100 - 100 * 128 / 255 = 49.8, 200 + 55 * 128 / 255 = 227.6
  Expect(o[0] == 60 && o[1] == 50 && o[2] == 228, "a = 128 blends per channel (SRC_ALPHA / 1 - SRC_ALPHA)");
  sp::Cache none = c;
  none.match.clear();
  sp::FadeFrom(none, 128, o);
  Expect(o == c.base, "an arena without a match page keeps its base");
  // S103 FD: the alpha follows the GAME's page clock (PageAlpha255 on the fed t), through a no-entrance match, a replay rewind and a second match. `--plant own_fade_clock` replaces the rule with the
  // old one (a clock of ours, started at the first non-entrance frame, unsigned guest_frame - t0): the same asserts must FAIL (the check expects exit 1).
  {
    const bool plant = argc == 3 && std::strcmp(argv[1], "--plant") == 0 && std::strcmp(argv[2], "own_fade_clock") == 0;
    struct OldClock {
      bool on = false;
      uint64_t t0 = 0;
      int Alpha(uint64_t frame, bool entrance) {
        if (entrance) { on = false; return 255; }
        if (!on) on = true, t0 = frame;
        return sp::FadeAlpha255(frame - t0);
      }
    } old;
    // a frame = (guest frame, game t, running, entrance word, entrance look). The game: the page object exists from match load (t 0, not running through the ring intro, ~400 frames), the clock
    // runs from the match clock start; match 2 = a new object (t restarts at 0); a rewind = t drops to a lower value.
    const auto alpha = [&](uint64_t frame, bool have, float t, bool entw, bool entrance) {
      const int game = sp::PageAlpha255(have, entw, t);
      const int o = old.Alpha(frame, entrance);
      return plant ? o : game;
    };
    // the ring intro (no entrance in this match): frames 0..399, t stays 0 (not running)
    int a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0, a8 = 0;
    for (uint64_t f = 0; f < 400; ++f) a0 = alpha(f, true, 0.f, false, false);
    a1 = alpha(400, true, 4.f, false, false);              // the match clock starts: t = 4
    a2 = alpha(700, true, 304.f, false, false);            // clock + 300 (the stream's first fade draw)
    a3 = alpha(702, true, 306.f, false, false);
    a4 = alpha(1000, true, 604.f, false, false);           // past 600: page 01
    a5 = alpha(1100, true, 150.f, false, false);           // a replay rewind: t dropped, the frame number did not wrap
    a6 = alpha(50, true, 12.f, false, false);              // the guest frame went BACK (unsigned wrap in the old rule)
    a7 = alpha(5000, true, 0.f, false, true);              // second match: a new object at t 0 (this frame in an entrance: not faded)
    a8 = alpha(5400, true, 304.f, false, false);           // second match, clock + 300
    Expect(a0 == 0, "ring intro: the page clock has not started (t = 0): alpha 0 (page 00)");
    Expect(a1 == 1, "clock t = 4: alpha 1 (int(1.7))");
    Expect(a2 == 129 && a3 == 130, "clock t = 304 / 306: alpha 129 / 130 (the stream's own values)");
    Expect(a4 == 255, "clock t >= 600: page 01 (alpha 255)");
    Expect(a5 == 63, "rewind: t = 150 gives alpha int(255 * 150 / 600) = 63 again");
    Expect(a6 == 5, "a guest frame that went back: t = 12 gives alpha 5 (no unsigned wrap to 255)");
    Expect(a8 == 129, "second match: the game's t restarts, the fade restarts (alpha 129 at t = 304)");
    (void)a7;
    // the sweep 0 -> 600 ticks: alpha = int(255 t / 600) at every whole tick, monotone, never past 254 before 600
    int prev = 0;
    bool sweep_ok = true;
    for (int t = 0; t < 600; ++t) {
      const int a = sp::PageAlpha255(true, false, float(t));
      sweep_ok = sweep_ok && a == int(float(t) * (1.f / 600.f) * 255.f) && a >= prev && a <= 254;
      prev = a;
    }
    Expect(sweep_ok, "t sweep 0..599: alpha follows the game's formula, monotone");
    Expect(sp::PageAlpha255(true, false, 303.9f) == sp::PageAlpha255(true, false, 303.f), "t is truncated to a whole tick first (fctiwz)");
    Expect(sp::PageAlpha255(false, false, 100.f) == 255, "no clock row: the settled page (255)");
    Expect(sp::PageAlpha255(true, true, 400.f) == 0, "entrance word set: the draw passes t = 0.0");
    Expect(sp::PageAlpha255(true, false, -5.f) == 0 && sp::PageAlpha255(true, false, std::nanf("")) == 0, "t < 0 / NaN reads as 0");
    Expect(sp::PageAlpha255(true, false, 1250.f) == 255, "t >= 1200: nothing drawn, the target holds page 01 (255)");
  }
  std::printf("l8_unit: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
