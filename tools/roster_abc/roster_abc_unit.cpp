// S110 DLCRANK: host unit for src\roster_abc_core.h (the DOWNLOADED tab's alphabetical order). Made-up names, plus a few wrestler NAMES as plain test strings.
//   roster_abc_unit.exe     every check must PASS (exit 0; the last line is "PASS roster_abc_unit" or "FAIL roster_abc_unit").
// Built and run (with its planted twins) by tools\roster_abc\roster_abc_selftest.py.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "roster_abc_core.h"

namespace ra = wwe2k14::roster_abc;
using Items = std::vector<ra::AbcItem>;

static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_fail;
    std::printf("FAIL  %s\n", what.c_str());
  }
}

static ra::AbcItem D(const char* n) { return ra::AbcItem{n, true}; }   // a downloaded row (a DLC row or an import)
static ra::AbcItem S(const char* n) { return ra::AbcItem{n, false}; }  // a stock row or a created superstar

static bool IsPerm(const std::vector<int>& p, size_t n) {
  if (p.size() != n) return false;
  std::vector<int> s = p;
  std::sort(s.begin(), s.end());
  for (size_t i = 0; i < n; ++i)
    if (s[i] != int(i)) return false;
  return true;
}

// The names as the list reads after the order is applied; also checks that the order is a permutation (no entry lost, none doubled).
static std::string Joined(const Items& items) {
  const std::vector<int> p = ra::AbcOrder(items);
  Check(IsPerm(p, items.size()), "the order is a permutation");
  std::string s;
  for (size_t i = 0; i < p.size() && i < items.size(); ++i) {
    if (i) s += "|";
    s += items[p[i] < int(items.size()) && p[i] >= 0 ? p[i] : 0].name;
  }
  return s;
}

static void Cases() {
  // stock rows untouched: the non-downloaded entries keep their positions, whatever their names
  {
    const Items it = {S("ZED"), D("BOB"), S("AAA"), D("AMY")};
    const std::vector<int> p = ra::AbcOrder(it);
    Check(p.size() == 4 && p[0] == 0 && p[2] == 2, "stock rows untouched: a non-downloaded entry never moves");
    Check(Joined(it) == "ZED|AMY|AAA|BOB", "stock rows untouched: the two downloaded entries swap places around them");
  }
  // DLC rows (pack order) and imports interleave into one order
  {
    const Items it = {D("BIG E LANGSTON"), D("FANDANGO"), D("UNDERTAKER (Biker)"), D("VIRGIL"), D("Bobby Lashley"), D("Maryse")};
    Check(Joined(it) == "BIG E LANGSTON|Bobby Lashley|FANDANGO|Maryse|UNDERTAKER (Biker)|VIRGIL", "DLC rows and imports interleaved into one order, case folded");
  }
  // THE GIANT sorts under G
  {
    const Items it = {D("FANDANGO"), D("JAKE ROBERTS"), D("THE GIANT (nova)"), D("DUSTY RHODES")};
    Check(Joined(it) == "DUSTY RHODES|FANDANGO|THE GIANT (nova)|JAKE ROBERTS", "THE GIANT sorts under G (a leading THE is dropped)");
  }
  // n.W.o. before Outsiders for the same name
  {
    const Items it = {D("ROY BELL (Orbit)"), D("ROY BELL (nova)")};
    Check(Joined(it) == "ROY BELL (nova)|ROY BELL (Orbit)", "a lower-case (nova) variant before the (Orbit) variant of the same name");
    const Items it2 = {D("AMY COLE (Orbit)"), D("AMY COLE (nova)"), D("AMY DALE (nova)")};
    Check(Joined(it2) == "AMY COLE (nova)|AMY COLE (Orbit)|AMY DALE (nova)", "variants of one name stay together, both before the next name");
  }
  // digits and symbols first
  {
    const Items it = {D("ABEL"), D("3 COUNT"), D("-DASH"), D("zed")};
    const std::string j = Joined(it);
    Check(j == "3 COUNT|-DASH|ABEL|zed" || j == "-DASH|3 COUNT|ABEL|zed", "digits and symbols sort before the letters");
    Check(Joined({D("BETA"), D("9 LIVES"), D("ALPHA")}).substr(0, 7) == "9 LIVES", "a digit-led name is first");
  }
  // equal names keep list order (the two rows are told apart by a trailing blank-free tag kept in a parallel field: use differing case)
  {
    const Items it = {D("SAME"), D("ALPHA"), D("same")};
    const std::vector<int> p = ra::AbcOrder(it);
    Check(p.size() == 3 && p[0] == 1 && p[1] == 0 && p[2] == 2, "ties keep list order (SAME, same: the earlier row first)");
    const Items it2 = {D("same"), D("ALPHA"), D("SAME")};
    const std::vector<int> p2 = ra::AbcOrder(it2);
    Check(p2.size() == 3 && p2[0] == 1 && p2[1] == 0 && p2[2] == 2, "ties keep list order (same, SAME: the earlier row first)");
  }
  // a created superstar (not downloaded) between two downloaded rows stays put
  {
    const Items it = {D("ZOE"), S("MIDDLE GUY"), D("ADA")};
    const std::vector<int> p = ra::AbcOrder(it);
    Check(p.size() == 3 && p[1] == 1, "a created superstar between two downloaded rows stays put");
    Check(Joined(it) == "ADA|MIDDLE GUY|ZOE", "the two downloaded rows swap around the created superstar");
  }
  // an empty name keeps its entry and sorts first among the downloaded
  {
    const Items it = {D("BETA"), S("KEEP"), D(""), D("ALPHA")};
    Check(Joined(it) == "|KEEP|ALPHA|BETA", "an empty name keeps its entry and sorts first among the downloaded");
  }
  // the identity when already sorted; nothing / one downloaded
  {
    const Items it = {S("Z"), D("ALPHA"), S("Y"), D("BETA"), D("GAMMA")};
    Check(ra::IsIdentity(ra::AbcOrder(it)), "already sorted: the identity");
    Check(ra::IsIdentity(ra::AbcOrder({})), "an empty list: the identity");
    Check(ra::IsIdentity(ra::AbcOrder({S("B"), S("A")})), "no downloaded entry: the identity");
    Check(ra::IsIdentity(ra::AbcOrder({S("B"), D("A")})), "one downloaded entry: the identity");
  }
  // a longer mixed list stays a permutation and is sorted among the downloaded
  {
    Items it;
    const char* names[] = {"QUINN", "PIA", "ROY", "ODE", "NED", "MAX", "LEE", "KAI", "JOY", "IAN", "HAL", "GUS", "FAY", "EVE", "DAN", "CAL", "BEA", "ABE"};
    for (int i = 0; i < 18; ++i) it.push_back(i % 4 == 3 ? S(names[i]) : D(names[i]));
    const std::vector<int> p = ra::AbcOrder(it);
    Check(IsPerm(p, it.size()), "a longer mixed list: a permutation");
    bool fixed = true, sorted = true;
    std::string prev;
    for (size_t i = 0; i < it.size(); ++i) {
      if (!it[i].downloaded) {
        if (p[i] != int(i)) fixed = false;
      } else {
        const std::string cur = it[p[i]].name;
        if (!prev.empty() && cur < prev) sorted = false;
        prev = cur;
      }
    }
    Check(fixed, "a longer mixed list: every non-downloaded entry in place");
    Check(sorted, "a longer mixed list: the downloaded entries ascend");
  }
}

int main() {
  Cases();
  std::printf("%s roster_abc_unit: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
