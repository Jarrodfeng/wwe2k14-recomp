// S102 G2: the two output levers' decisions (low_levers.h), proven with the real header. Exit 0 = all hold.
#include <cstdio>
#include "low_levers.h"
using namespace rdr_adapter::low_levers;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
  CHECK(OutputCapHeight(1280, true) == 1280);     // LOW, a sensible ask: applied
  CHECK(OutputCapHeight(1280, false) == 0);       // not LOW: never
  CHECK(OutputCapHeight(0, true) == 0);           // off
  CHECK(OutputCapHeight(100, true) == 0);         // smaller than a 720p picture: refused
  CHECK(OutputCapHeight(-5, true) == 0);
  CHECK(OutputCapHeight(100000, true) == 0);
  CHECK(PresentBilinear(true, true));
  CHECK(!PresentBilinear(true, false));           // STANDARD keeps SGSR
  CHECK(!PresentBilinear(false, true));           // lever off keeps SGSR
  return fails ? 1 : 0;
}
