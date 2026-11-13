// S101 lane 108: runs the game's own `!wm30 fire` parse rule and guard truth table (src/wm30_fire_args.h, the header src/wm30_fire.h calls) on each
// stdin line. Built and driven by tools/goto/fire_selftest.py (clang++ -std=c++17 -I src tools/goto/fire_test.cpp). No game, no SDK.
//   parse <text...>                       -> "ERR <text>" | "FIRE <bank> <force 0/1> <actor> <mode>"
//   route <text...>                       -> "ROUTE 0/1" (IsFireCommand of the text)
//   verdict started in_game object g472 g468 flag   (six integers) -> "GO" | "<refusal text>"
#include <iostream>
#include <sstream>
#include <string>

#include "wm30_fire_args.h"

int main() {
  using namespace wwe2k14::wm30_fire;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "parse") {
      std::string rest;
      std::getline(in, rest);
      FireArgs a;
      const std::string err = ParseFireArgs(rest, a);
      if (!err.empty()) {
        std::cout << "ERR " << err << "\n";
      } else {
        std::cout << "FIRE " << a.bank << " " << (a.force ? 1 : 0) << " " << a.actor << " " << a.mode << "\n";
      }
    } else if (verb == "route") {
      std::string rest;
      std::getline(in, rest);
      std::cout << "ROUTE " << (IsFireCommand(rest) ? 1 : 0) << "\n";
    } else if (verb == "verdict") {
      FireInputs f;
      int started, in_game, object, flag;
      unsigned g472, g468, held;
      int clock_value;
      in >> started >> in_game >> object >> g472 >> g468 >> flag >> clock_value >> held;  // lane 144: clock_value, clock_held
      f.started = started;
      f.in_game = in_game;
      f.have_object = object;
      f.g472 = g472;
      f.g468 = g468;
      f.flag = flag;
      f.clock = clock_value;  // lane 144
      f.clock_held = held;    // lane 144
      const std::string v = FireVerdict(f);
      std::cout << (v.empty() ? "GO" : v) << "\n";
    } else {
      std::cout << "ERR unknown test verb\n";
    }
  }
  return 0;
}
