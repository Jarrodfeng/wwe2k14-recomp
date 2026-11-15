// S102 lane G2c unit test: display_pick.h (the pick rule of wwe2k14_display_720p_mode=auto:N). Built by tools\g2c\g2c_check.py against the real header and against planted
// twins that must FAIL it. No Windows, no window.
#include <cstdio>
#include "display_pick.h"

using namespace wwe2k14::display_pick;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
  {  // the Surface Pro 6: panel 2736x1824 (3:2) at 60. Asked 912 -> 1368x912 (exactly half).
    const Mode list[] = {{2736, 1824, 60}, {1920, 1280, 60}, {1824, 1216, 60}, {1368, 912, 60}, {1280, 720, 60}, {1024, 768, 60}, {912, 608, 60}, {1368, 912, 30}};
    Mode m;
    CHECK(Pick(list, 8, 2736, 1824, 60, 912, m) && m.w == 1368 && m.h == 912 && m.hz == 60);
    CHECK(Pick(list, 8, 2736, 1824, 60, 913, m) && m.w == 1824 && m.h == 1216);   // smallest AT OR ABOVE: 913 skips 912
    CHECK(Pick(list, 8, 2736, 1824, 60, 1, m) && m.w == 912 && m.h == 608);       // a tiny ask takes the smallest panel-aspect mode
    CHECK(Pick(list, 8, 2736, 1824, 60, 1280, m) && m.w == 1920 && m.h == 1280);  // 1280x720 is 16:9, never taken for the 3:2 panel
    CHECK(!Pick(list, 8, 2736, 1824, 60, 1825, m));                               // above the panel: nothing
    CHECK(!Pick(list, 8, 2736, 1824, 60, 1500, m));                               // only the panel's own mode is left above 1216: no change
  }
  {  // no square-pixel match: only 16:9 and 4:3 modes, and 1366x912 which is NOT 3:2 (the cross-multiply says so)
    const Mode list[] = {{1920, 1080, 60}, {1280, 720, 60}, {1024, 768, 60}, {1366, 912, 60}};
    Mode m;
    CHECK(!Pick(list, 4, 2736, 1824, 60, 600, m));
  }
  {  // only higher modes than asked, and only the panel's own
    const Mode list[] = {{2736, 1824, 60}, {1920, 1280, 60}};
    Mode m;
    CHECK(Pick(list, 2, 2736, 1824, 60, 1000, m) && m.h == 1280);
    CHECK(!Pick(list, 2, 2736, 1824, 60, 1281, m));
    CHECK(!Pick(list, 0, 2736, 1824, 60, 912, m));
  }
  {  // several refresh rates: only the current one counts
    const Mode list[] = {{1368, 912, 30}, {1368, 912, 144}, {1824, 1216, 60}, {1368, 912, 60}, {1368, 912, 60}};
    Mode m;
    CHECK(Pick(list, 5, 2736, 1824, 60, 900, m) && m.h == 912 && m.hz == 60);  // duplicates (centred / stretched twins) are fine
    CHECK(Pick(list, 5, 2736, 1824, 144, 900, m) && m.h == 912 && m.hz == 144);
    CHECK(!Pick(list, 5, 2736, 1824, 120, 900, m));
    const Mode only_other[] = {{1368, 912, 30}};
    CHECK(!Pick(only_other, 1, 2736, 1824, 60, 900, m));
  }
  {  // a 16:9 panel and bad input
    const Mode list[] = {{3840, 2160, 60}, {2560, 1440, 60}, {1920, 1080, 60}, {1280, 720, 60}, {1368, 912, 60}};
    Mode m;
    CHECK(Pick(list, 5, 3840, 2160, 60, 700, m) && m.w == 1280 && m.h == 720);
    CHECK(Pick(list, 5, 3840, 2160, 60, 1000, m) && m.h == 1080);
    CHECK(!Pick(list, 5, 0, 2160, 60, 700, m) && !Pick(list, 5, 3840, 2160, 60, 0, m));
  }
  {  // the value parser
    CHECK(ParseAuto("auto:912") == 912 && ParseAuto("auto:1") == 1);
    CHECK(ParseAuto("1280x720") == 0 && ParseAuto("") == 0 && ParseAuto("auto:") == 0 && ParseAuto("auto:9x") == 0 && ParseAuto("auto:-5") == 0 && ParseAuto("AUTO:912") == 0);
    CHECK(ParseAuto(nullptr) == 0 && ParseAuto("auto:99999999999") == 0);
  }
  std::printf(fails ? "pick_test FAILED %d\n" : "pick_test ok\n", fails);
  return fails ? 1 : 0;
}
