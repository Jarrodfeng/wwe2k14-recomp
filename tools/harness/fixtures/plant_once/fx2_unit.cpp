// second synthetic unit of tools/harness/plant_once_selftest.py: exit 1 on any failed check. Its verdict is the exit code alone (the seats
// scripts' rule), so a crash would read as caught
#include <cstdio>
#include <fx2.h>  // angle form: a quote-include would find the shipped copy beside this file first

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  if (!ok) {
    ++g_fail;
    std::printf("FAIL %s\n", what);
  }
}

int main() {
  Check(fx2::Crashy(5) == 5 && fx2::Crashy(-1) == 0, "crashy");
  Check(fx2::Param(5) == 5, "param");
  Check(fx2::Brace(150) == 100 && fx2::Brace(5) == 5, "brace");
  Check(fx2::Uninit(5) == 5, "uninit");
  Check(fx2::Seven() == 7, "seven");
  return g_fail ? 1 : 0;
}
