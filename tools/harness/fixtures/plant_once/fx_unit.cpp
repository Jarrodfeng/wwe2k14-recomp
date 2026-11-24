// synthetic unit of tools/harness/plant_once_selftest.py: prints FAIL lines and a summary, exit code 1 on any failure
#include <cstdio>
#include <vector>
#include <fx.h>  // angle form: a quote-include would find the shipped copy beside this file first

static_assert(fx::Twice(3) > 0, "Twice is a constant expression");

static int g_fails = 0;
static void Check(bool ok, const char* what) {
  if (!ok) {
    ++g_fails;
    std::printf("FAIL %s\n", what);
  }
}

int main() {
  Check(fx::Clamp(15) == 10, "clamp above");
  Check(fx::Clamp(5) == 5, "clamp inside");
  Check(fx::Clamp(-3) == 0, "clamp below");
  Check(fx::Sum({1, 2, 3}) == 6, "sum");
  Check(fx::Add(2, 3) == 5, "add int");
  Check(fx::Add(1.5, 1.5) == 3.0, "add double");
  Check(fx::Twice(4) == 8, "twice");
  fx::Box b;
  Check(b.total == 1 && b.Area(3) == 3, "box");
  (void)fx::Unchecked(2);
  std::printf("%s fx_unit\n", g_fails ? "FAIL" : "PASS");
  return g_fails ? 1 : 0;
}
