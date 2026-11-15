// S102 lane G1 unit test: the pure decisions of src\renderer_adapter\low_levers.h and src\renderer\present_clear.h. Exit 0 = all pass.
// tools\g1\g1_check.py compiles it against the real headers and against planted-fault copies of them (each must fail).
#include <cstdio>
#include <map>
#include <string>

#include "low_levers.h"
#include "present_clear.h"

using namespace rdr_adapter::low_levers;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static FlagReader Flags(std::map<std::string, std::string> m) {
  return [m](const char* n) { auto it = m.find(n); return it == m.end() ? std::string() : it->second; };
}

int main() {
  // IsLow: the resolved tier (the quality note); the tier's flag only when there is no note.
  CHECK(!IsLow(Flags({})));
  CHECK(!IsLow(Flags({{"gpu_native_crowd_pose_half", "false"}})));
  CHECK(IsLow(Flags({{"gpu_native_crowd_pose_half", "true"}})));                                    // no note: the tier's flag
  CHECK(IsLow(Flags({{"wwe2k14_quality_note", "LOW_auto_phone"}})));                                // a phone writes no LOW flag (rung 2: shadows off)
  CHECK(IsLow(Flags({{"wwe2k14_quality_note", "LOW_chosen"}})));
  CHECK(IsLow(Flags({{"wwe2k14_quality_note", "LOW_auto_integrated"}, {"gpu_shadow_half_rate", "false"}})));
  CHECK(!IsLow(Flags({{"wwe2k14_quality_note", "STANDARD_chosen"}, {"gpu_shadow_half_rate", "true"}})));  // STANDARD passing the flag by hand
  CHECK(!IsLow(Flags({{"wwe2k14_quality_note", "STANDARD_original_look"}, {"gpu_native_crowd_pose_half", "true"}})));  // the note wins over the flag
  CHECK(!IsLow(Flags({{"gpu_shadow_half_rate", "true"}})));                                          // the shadow flag alone never means LOW
  CHECK(!IsLow(Flags({{"wwe2k14_quality_note", "XLOW_chosen"}})));                                   // a prefix, not a substring

  // Lever 1: shadow map size.
  CHECK(ShadowMapSize(true, true, true, 512) == 256);    // on
  CHECK(ShadowMapSize(false, true, true, 512) == 512);   // switch off: the lever never fires
  CHECK(ShadowMapSize(true, false, true, 512) == 512);   // STANDARD: untouched
  CHECK(ShadowMapSize(true, true, false, 512) == 512);   // the maps are the stream's: nothing to resize
  CHECK(ShadowMapSize(true, true, true, 128) == 128);    // never raises a smaller ask
  CHECK(ShadowMapSize(true, true, true, 2048) == 256);

  // Lever 2: present clear.
  CHECK(PresentNoClear(true, true));
  CHECK(!PresentNoClear(true, false));
  CHECK(!PresentNoClear(false, true));

  // Lever 3: copy alias.
  CHECK(CopyAliasFor(true, true, 0, 1).mode == 2 && CopyAliasFor(true, true, 0, 1).classes == 11u);  // bake | shadow | post
  CHECK(CopyAliasFor(true, true, 1, 1).mode == 2);
  CHECK(CopyAliasFor(true, true, 2, 5).classes == (11u | 5u));   // a player already in drop mode keeps their own classes
  for (int mode = 0; mode < 2; ++mode)                             // the lever never sets scene colour (16) or other (32): lane 190's notes
    for (uint32_t asked = 0; asked < 64; ++asked) CHECK((CopyAliasFor(true, true, mode, asked).classes & (16u | 32u)) == 0);
  CHECK(CopyAliasFor(false, true, 0, 1).mode == 0 && CopyAliasFor(false, true, 0, 1).classes == 1u);
  CHECK(CopyAliasFor(true, false, 1, 1).mode == 1);

  // Present bars (lever 2's arithmetic). Panels: 2736x1824 Surface, 1920x1080, a 21:9 window, a phone turned.
  rdr::PresentBar b[4];
  CHECK(rdr::PresentBars(2736, 1824, 0, 0, 2736, 1824, b) == 0);                       // covers the panel: nothing to clear
  CHECK(rdr::PresentBars(1920, 1080, 0, 0, 1920, 1080, b) == 0);
  {  // 16:9 on the 3:2 Surface: 2736 x 1539 centred -> two bars top and bottom, 142 / 143 high
    const int n = rdr::PresentBars(2736, 1824, 0, 142, 2736, 1539, b);
    CHECK(n == 2);
    CHECK(b[0].x == 0 && b[0].y == 0 && b[0].w == 2736 && b[0].h == 142);
    CHECK(b[1].x == 0 && b[1].y == 142 + 1539 && b[1].w == 2736 && b[1].h == 1824 - 142 - 1539);
  }
  {  // pillarbox: 1440x1080 in 1920x1080: bars left and right
    const int n = rdr::PresentBars(1920, 1080, 240, 0, 1440, 1080, b);
    CHECK(n == 2);
    CHECK(b[0].x == 0 && b[0].w == 240 && b[0].h == 1080);
    CHECK(b[1].x == 1680 && b[1].w == 240 && b[1].h == 1080);
  }
  {  // every pixel of the panel is either in the viewport or in exactly one bar (the invariant: nothing left unwritten, nothing cleared twice).
     // Every viewport inside a small panel (including the one-pixel bars at the edges), so an off-by-one anywhere shows.
    const int W = 9, H = 7;
    int bad = 0, tried = 0;
    for (int vx = 0; vx < W; ++vx)
      for (int vy = 0; vy < H; ++vy)
        for (int vw = 1; vx + vw <= W; ++vw)
          for (int vh = 1; vy + vh <= H; ++vh) {
            const int n = rdr::PresentBars(W, H, vx, vy, vw, vh, b);
            int cover[H][W] = {};
            for (int i = 0; i < n; ++i)
              for (uint32_t y = 0; y < b[i].h; ++y)
                for (uint32_t x = 0; x < b[i].w; ++x) ++cover[b[i].y + y][b[i].x + x];
            ++tried;
            for (int y = 0; y < H; ++y)
              for (int x = 0; x < W; ++x) {
                const bool inside = x >= vx && x < vx + vw && y >= vy && y < vy + vh;
                if (cover[y][x] != (inside ? 0 : 1)) ++bad;
              }
            if (vx == 0 && vy == 0 && vw == W && vh == H && n != 0) ++bad;  // covers the panel: no clear at all
          }
    CHECK(tried > 1000 && bad == 0);
  }
  CHECK(rdr::PresentBars(100, 100, 0, 0, 0, 0, b) == 1 && b[0].w == 100 && b[0].h == 100);  // empty viewport: clear it all
  CHECK(rdr::PresentBars(100, 100, 150, 0, 50, 50, b) == 1);                                // outside

  std::printf(fails ? "G1 UNIT: %d FAILED\n" : "G1 UNIT: all passed\n", fails);
  return fails ? 1 : 0;
}
