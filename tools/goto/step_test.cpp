// S101 lane 121: runs the game's own `!wm30 step` parse rule and guard truth table (src/wm30_step_args.h, the header src/wm30_step.h calls) on each
// stdin line. Built and driven by tools/goto/step_selftest.py (clang++ -std=c++17 -I <dir with wm30_step_args.h> tools/goto/step_test.cpp). No game, no SDK.
//   parse <text...>   -> "ERR <text>" | "STEP <k> <force 0/1> <list_given 0/1> <entry_free 0/1> <steps comma list or ->"
//   route <text...>   -> "ROUTE 0/1" (IsStepCommand of the text)
//   verdict started in_game clock object g472 g468 list_given in_list entry_free force step shared_zero clock_value clock_held   (fourteen integers; lane 144 added the last two) -> "GO" | "<refusal text>"
#include <iostream>
#include <sstream>
#include <string>

#include "wm30_step_args.h"

int main() {
  using namespace wwe2k14::wm30_step;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "parse") {
      std::string rest;
      std::getline(in, rest);
      StepArgs a;
      const std::string err = ParseStepArgs(rest, a);
      if (!err.empty()) {
        std::cout << "ERR " << err << "\n";
      } else {
        std::string list;
        for (size_t i = 0; i < a.steps.size(); ++i) list += (i ? "," : "") + std::to_string(a.steps[i]);
        std::cout << "STEP " << a.step << " " << (a.force ? 1 : 0) << " " << (a.list_given ? 1 : 0) << " " << (a.entry_free ? 1 : 0) << " " << (list.empty() ? "-" : list) << "\n";
      }
    } else if (verb == "route") {
      std::string rest;
      std::getline(in, rest);
      std::cout << "ROUTE " << (IsStepCommand(rest) ? 1 : 0) << "\n";
    } else if (verb == "verdict") {
      StepInputs s;
      int started, in_game, clock, object, list_given, in_list, entry_free, force, step, shared_zero;
      unsigned g472, g468, held;
      int clock_value;
      in >> started >> in_game >> clock >> object >> g472 >> g468 >> list_given >> in_list >> entry_free >> force >> step >> shared_zero >> clock_value >> held;
      s.started = started;
      s.in_game = in_game;
      s.clock_started = clock;
      s.have_object = object;
      s.g472 = g472;
      s.g468 = g468;
      s.list_given = list_given;
      s.in_list = in_list;
      s.entry_free = entry_free;
      s.force = force;
      s.step = step;
      s.shared_zero = shared_zero;
      s.clock = clock_value;  // lane 144
      s.clock_held = held;    // lane 144
      const std::string v = StepVerdict(s);
      std::cout << (v.empty() ? "GO" : v) << "\n";
    } else {
      std::cout << "ERR unknown test verb\n";
    }
  }
  return 0;
}
