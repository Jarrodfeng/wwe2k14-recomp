// S110 LETTER: host unit for src\letter_jump_core.h (the initial of a name; which row a trigger press goes to). Made-up names only, no game data.
//   letter_jump_unit.exe     every check must PASS (exit 0; the last line is "PASS letter_jump_unit" or "FAIL letter_jump_unit").
// Built and run (with its planted twins) by tools\letter_jump\letter_jump_selftest.py.
#include <cstdio>
#include <string>
#include <vector>

#include "letter_jump_core.h"

namespace lj = wwe2k14::letter_jump;

static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_fail;
    std::printf("FAIL  %s\n", what.c_str());
  }
}

// A list from names, in list order; `off` = indexes that are not selectable.
static std::vector<lj::Row> Make(const std::vector<std::string>& names, const std::vector<int>& off = {}) {
  std::vector<lj::Row> rows;
  for (size_t i = 0; i < names.size(); ++i) {
    lj::Row r;
    r.index = int(i);
    r.letter = lj::Initial(names[i]);
    r.selectable = true;
    for (int o : off)
      if (o == int(i)) r.selectable = false;
    rows.push_back(r);
  }
  return rows;
}

static void InitialCases() {
  Check(lj::Initial("ADAM") == 'A', "Initial: plain upper case");
  Check(lj::Initial("adam") == 'A', "Initial: case folded");
  Check(lj::Initial("THE BARON") == 'B', "Initial: a leading THE is dropped");
  Check(lj::Initial("the baron") == 'B', "Initial: a leading the is dropped, any case");
  Check(lj::Initial("THEO") == 'T', "Initial: THEO is not 'THE '");
  Check(lj::Initial("THEODORE") == 'T', "Initial: THEODORE is not 'THE ' (a longer word that starts with THE)");
  Check(lj::Initial("THE") == 'T', "Initial: THE alone stays under T");
  Check(lj::Initial("THE ") == 'T', "Initial: THE and a blank stays under T");
  Check(lj::Initial("THE  BARON") == 'B', "Initial: blanks after THE are skipped");
  Check(lj::Initial("  CARL") == 'C', "Initial: leading blanks skipped");
  Check(lj::Initial("\xC9" "MILE") == 'E', "Initial: Latin-1 E acute -> E");
  Check(lj::Initial("\xE9" "mile") == 'E', "Initial: Latin-1 e acute -> E");
  Check(lj::Initial("\xC3\x89" "MILE") == 'E', "Initial: UTF-8 E acute -> E");
  // review S110 LETTER: a UTF-8 lead byte outside Latin-1 is not a Latin-1 letter (0xC5 alone is A-ring, 0xE2 alone is a-circumflex)
  Check(lj::Initial("\xC5\xA0" "ARKA") == '#', "Initial: UTF-8 S caron (U+0160) is not read as the Latin-1 bytes A-ring + nbsp");
  Check(lj::Initial("\xE2\x80\x9C" "ACE\xE2\x80\x9D") == '#', "Initial: a UTF-8 curly quote (3 bytes) is a symbol, not a-circumflex");
  Check(lj::Initial("\xC4\x86" "ARL") == '#', "Initial: UTF-8 C acute (U+0106) is not read as the Latin-1 bytes A-diaeresis + a control byte");
  Check(lj::Initial("\xC3") == 'A',"Initial: a lone Latin-1 A-tilde at the end is still a Latin-1 letter");
  Check(lj::Initial("\xD1" "ANDU") == 'N', "Initial: Latin-1 N tilde -> N");
  Check(lj::Initial("\xD6" "ZIL") == 'O', "Initial: Latin-1 O diaeresis -> O");
  Check(lj::Initial("\xFF" "AN") == 'Y', "Initial: Latin-1 y diaeresis -> Y");
  Check(lj::Initial("\xDF" "ARA") == 'S', "Initial: sharp s -> S");
  Check(lj::Initial("\xC7" "ARLOS") == 'C', "Initial: C cedilla -> C");
  Check(lj::Initial("3 COUNT") == '#', "Initial: a digit -> the # bucket");
  Check(lj::Initial("$ILVER") == '#', "Initial: a symbol -> the # bucket");
  Check(lj::Initial("\xD7" "X") == '#', "Initial: the multiplication sign is not a letter");
  Check(lj::Initial("") == 0, "Initial: empty -> no letter");
  Check(lj::Initial("   ") == 0, "Initial: blank -> no letter");
  Check(lj::Initial("#" "1") < lj::Initial("ABE"), "Initial: # sorts before A");
}

static void NextRowCases() {
  // a stock-shaped list: alphabetical (THE ignored)
  const auto stock = Make({"ABEL", "ALAN", "BARRY", "BORIS", "THE CALLER", "DAVE", "DOUG", "ELLIS"});
  Check(lj::NextRow(stock, 0, +1) == 2, "RT from A (first row) -> first B row");
  Check(lj::NextRow(stock, 1, +1) == 2, "RT from the middle of A -> first B row");
  Check(lj::NextRow(stock, 2, +1) == 4, "RT from B -> C (THE CALLER under C)");
  Check(lj::NextRow(stock, 4, +1) == 5, "RT from C -> D");
  Check(lj::NextRow(stock, 6, +1) == 7, "RT from the middle of D -> E");
  Check(lj::NextRow(stock, 7, +1) == 0, "RT from the last letter wraps to the first row");
  Check(lj::NextRow(stock, 0, -1) == 7, "LT from the first row of the first letter wraps to the last letter");
  Check(lj::NextRow(stock, 2, -1) == 0, "LT from the first row of B -> first row of A");
  Check(lj::NextRow(stock, 3, -1) == 2, "LT from the middle of B -> the first row of B (the middle-of-run rule)");
  Check(lj::NextRow(stock, 1, -1) == 0, "LT from the middle of A -> the first row of A");
  Check(lj::NextRow(stock, 5, -1) == 4, "LT from D (first row) -> C");

  // letters absent are skipped
  const auto gap = Make({"AAA", "CCC", "FFF"});
  Check(lj::NextRow(gap, 0, +1) == 1, "RT skips absent letters (A -> C)");
  Check(lj::NextRow(gap, 1, +1) == 2, "RT skips absent letters (C -> F)");
  Check(lj::NextRow(gap, 2, -1) == 1, "LT skips absent letters (F -> C)");

  // unselectable rows are skipped; a letter whose rows are all unselectable is absent
  const auto locked = Make({"ABEL", "BARRY", "BORIS", "CARL", "DAVE"}, {1, 3});
  Check(lj::NextRow(locked, 0, +1) == 2, "RT: the first selectable row of B (the locked first B is skipped)");
  Check(lj::NextRow(locked, 2, +1) == 4, "RT: a letter with only locked rows (C) is skipped");
  Check(lj::NextRow(locked, 4, -1) == 2, "LT: the all-locked letter C is skipped");
  Check(lj::NextRow(locked, 3, +1) == 4, "RT from a locked row (the cursor can sit anywhere) -> the next letter present");
  Check(lj::NextRow(locked, 3, -1) == 2, "LT from a locked row of an absent letter -> the previous letter present");

  // one row only; every row the same letter
  Check(lj::NextRow(Make({"ONLY"}), 0, +1) == -1, "a list of one: RT does nothing");
  Check(lj::NextRow(Make({"ONLY"}), 0, -1) == -1, "a list of one: LT does nothing");
  const auto same = Make({"ABEL", "ALAN", "AMY"});
  Check(lj::NextRow(same, 0, +1) == -1, "all rows the same letter: RT does nothing");
  Check(lj::NextRow(same, 1, +1) == -1, "all rows the same letter (middle): RT does nothing");
  Check(lj::NextRow(same, 0, -1) == -1, "all rows the same letter: LT does nothing");
  Check(lj::NextRow(same, 2, -1) == -1, "all rows the same letter (last): LT does nothing");
  Check(lj::NextRow(Make({"AAA", "BBB"}, {1}), 0, +1) == -1, "one selectable row left: nothing to do");
  Check(lj::NextRow({}, 0, +1) == -1, "an empty list: nothing to do");

  // rows with no letter (an empty name) are never a target
  const auto blank = Make({"AAA", "", "BBB"});
  Check(lj::NextRow(blank, 0, +1) == 2, "RT: a row with no letter is skipped");

  // two alphabetical runs back to back, as the DOWNLOADED tab is (the DLC rows, then the imports)
  const auto two = Make({"ARNO", "BRAD", "CHAD", "DREW", "ALEX", "BETH", "FINN"});
  Check(lj::NextRow(two, 0, +1) == 1, "two runs: RT from A (run 1) -> B, first in list order (run 1)");
  Check(lj::NextRow(two, 4, +1) == 1, "two runs: RT from A in run 2 -> B, the first B in LIST order (run 1)");
  Check(lj::NextRow(two, 5, +1) == 2, "two runs: RT from B in run 2 -> C (run 1)");
  Check(lj::NextRow(two, 3, +1) == 6, "two runs: RT from D -> F (the only F, run 2)");
  Check(lj::NextRow(two, 6, +1) == 0, "two runs: RT from the last letter wraps to the first A");
  Check(lj::NextRow(two, 4, -1) == 0, "two runs: LT from the second A -> the first A (the current letter's first row)");
  Check(lj::NextRow(two, 0, -1) == 6, "two runs: LT from the first A wraps to F");
  Check(lj::NextRow(two, 5, -1) == 1, "two runs: LT from B in run 2 -> the first B (run 1)");
  Check(lj::NextRow(two, 1, -1) == 0, "two runs: LT from the first B -> A");

  // a symbol bucket sorts first
  const auto sym = Make({"3 COUNT", "ABEL", "BARRY"});
  Check(lj::NextRow(sym, 2, +1) == 0, "# is the first letter: RT from the last wraps to the # row");
  Check(lj::NextRow(sym, 1, -1) == 0, "# is before A: LT from A -> the # row");
  Check(lj::NextRow(sym, 0, -1) == 2, "LT from the # row wraps to the last letter");
}

int main() {
  InitialCases();
  NextRowCases();
  std::printf("%s letter_jump_unit: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
