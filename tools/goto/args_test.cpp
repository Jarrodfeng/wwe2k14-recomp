// S101 goto: runs the game's own `!wm30` parse rule (src/wm30_args.h, the header src/wm30_jump.h's LiveCommand calls) on each stdin line.
// Built and driven by tools/goto/selftest.py (clang++ -std=c++17 -I src tools/goto/args_test.cpp). No game, no SDK.
// Output, one line per input line: "OFF" | "ARM <n> <code>" | "ERR <text>".
#include <iostream>
#include <string>

#include "wm30_args.h"

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    wwe2k14::wm30::ArmArgs a;
    const std::string err = wwe2k14::wm30::ParseArmArgs(line, a);
    if (!err.empty()) std::cout << "ERR " << err << "\n";
    else if (a.off) std::cout << "OFF\n";
    else std::cout << "ARM " << a.n << " " << a.code << "\n";
    // The packed word round-trips (the hook reads it back with ArmMatch / ArmCode).
    if (err.empty() && !a.off) {
      const int64_t v = wwe2k14::wm30::PackArm(a.n, a.code);
      if (v == wwe2k14::wm30::kNotArmed || wwe2k14::wm30::ArmMatch(v) != a.n || wwe2k14::wm30::ArmCode(v) != a.code) {
        std::cout << "ERR pack round trip failed\n";
      }
    }
  }
  return 0;
}
