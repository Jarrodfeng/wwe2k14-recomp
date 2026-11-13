// S101 lane 32: runs the game's own `!scene` parse rule (src/scene_director_args.h, the header src/scene_director.h's LiveCommand calls) on each
// stdin line. Built and driven by tools/goto/scene_selftest.py (clang++ -std=c++17 -I src tools/goto/scene_args_test.cpp). No game, no SDK.
// Output, one line per input line:
//   "STAGE" | "LEAVE" (S101 lane 72) | "PLAY <id> <arena> <n> (<char> <attire>)..." (S101 lane 38) | "OFF" | "MENU <id> <force> <mode>" | "MID <n> <id>... <force>" | "AFTER <n> <id>... <force>" | "VICTORY <id> <layout> <default_layout>" | "ERR <text>"
#include <iostream>
#include <string>

#include "scene_director_args.h"

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    wwe2k14::scene::SceneArgs a;
    const std::string err = wwe2k14::scene::ParseSceneArgs(line, a);
    using wwe2k14::scene::Verb;
    if (!err.empty()) {
      std::cout << "ERR " << err << "\n";
    } else if (a.verb == Verb::kOff) {
      std::cout << "OFF\n";
    } else if (a.verb == Verb::kStage) {
      std::cout << "STAGE\n";
    } else if (a.verb == Verb::kLeave) {  // S101 lane 72
      std::cout << "LEAVE\n";
    } else if (a.verb == Verb::kPlay) {  // S101 lane 38: "PLAY <id> <arena> <n> (<char> <attire>)..."
      std::cout << "PLAY " << a.ids[0] << " " << a.arena << " " << a.nperf;
      for (int i = 0; i < a.nperf; ++i) std::cout << " " << a.perf_char[i] << " " << a.perf_attire[i];
      std::cout << "\n";
    } else if (a.verb == Verb::kMenu) {
      std::cout << "MENU " << a.ids[0] << " " << (a.force ? 1 : 0) << " " << a.mode << "\n";
    } else if (a.verb == Verb::kMid || a.verb == Verb::kAfter) {
      std::cout << (a.verb == Verb::kMid ? "MID " : "AFTER ") << a.n;
      for (int i = 0; i < a.n; ++i) std::cout << " " << a.ids[i];
      std::cout << " " << (a.force ? 1 : 0) << "\n";
    } else {
      std::cout << "VICTORY " << a.ids[0] << " " << a.layout << " " << wwe2k14::scene::DefaultVictoryLayout(a.ids[0]) << "\n";
    }
  }
  return 0;
}
