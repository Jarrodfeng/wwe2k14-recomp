// S102 CEN1: unit test of census_flush.h (no game). clang++ -std=c++17 -Werror census_flush_test.cpp ; `plant` = a site that forgets its generation (must FAIL).
#include "census_flush.h"
#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
static std::string g_last;
static void Sink(const char* s) { g_last = s; }
#define CHECK(c, m) do { if (!(c)) { std::printf("FAIL: %s\n", m); ++fails; } } while (0)

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "plant") == 0;
  census_flush::Site a, b;
  CHECK(!census_flush::Due(a), "nothing due before the first Raise");
  census_flush::Raise("wwe2k14_rdr_present_fetch", 1, Sink);
  CHECK(g_last == "[census] flush gen 1 phase end: wwe2k14_rdr_present_fetch=1", "the marker names the switch and the value that ended");
  CHECK(census_flush::Due(a), "site A is due after a Raise");
  if (plant) a.last.store(0);   // planted: the site forgets what it printed
  CHECK(!census_flush::Due(a), "site A is not due twice for one generation");
  CHECK(census_flush::Due(b), "site B (never asked) is still due for the same Raise");
  census_flush::Raise("a", 0, nullptr);
  census_flush::Raise("b", 5, Sink);
  CHECK(census_flush::Due(a) && !census_flush::Due(a), "two Raises before a look = one flush");
  CHECK(g_last == "[census] flush gen 3 phase end: b=5", "marker generation counts every Raise");
  census_flush::Raise("run", 0, Sink);
  CHECK(census_flush::Due(b) && g_last == "[census] flush gen 4 phase end: run end", "run end marker");
  std::printf(fails ? "census_flush: %d FAILED\n" : "census_flush: all checks passed\n", fails);
  return fails ? 1 : 0;
}
