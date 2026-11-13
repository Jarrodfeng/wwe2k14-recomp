// S101 lane 111: the shipped CarriedSetting / HeaderLine / ParseFlagLine (src/replay_carried.h, the code src/replay.h writes and reads a
// recording's header with) driven from stdin. Built and run by tools/goto/harvest_selftest.py (clang++ -std=c++17 -I src). No game, no SDK.
//   write FILE   : writes a recording header with the place cvars among other settings through HeaderLine (what OpenRecord does)
//   read FILE    : reads a header with ParseFlagLine (what Load does) and prints "flag NAME = VALUE" per carried line
//   roundtrip    : write to a temp stream, read back, require equality; prints OK or a diff
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "replay_carried.h"

using wwe2k14::replay::CarriedSetting;
using wwe2k14::replay::HeaderLine;
using wwe2k14::replay::ParseFlagLine;

static std::vector<std::pair<std::string, std::string>> Settings() {
  return {{"wwe2k14_match", "type=0;p=343:-1,324:-1;arena=4;pace=1"},
          {"wwe2k14_match_go", "1"},
          {"wwe2k14_wm30", "3"},
          {"wwe2k14_wm30_auto", "false"},
          {"wwe2k14_scene_victory", "6343"},
          {"wwe2k14_fixed_seed", "1234"},       // not carried (a comment line)
          {"det_frames", "true"},                // carried (old prefix)
          {"wwe2k14_match_compare", "x"},        // NOT a place cvar: must not ride along on the exact names
          {"wwe2k14_launch_poke", "1"}};         // a diagnosis option of a carried family: not carried
}

static std::string Header() {
  std::string h = "# wwe2k14 replay v2\nname t\nseed 5\ndlc none\n";
  for (auto& kv : Settings()) h += HeaderLine(kv.first, kv.second, false);
  return h;
}

static std::vector<std::pair<std::string, std::string>> Read(std::istream& in) {
  std::vector<std::pair<std::string, std::string>> out;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::pair<std::string, std::string> kv;
    if (ParseFlagLine(line, kv)) out.push_back(kv);
  }
  return out;
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "write" && argc > 2) {
    std::ofstream(argv[2], std::ios::binary) << Header();
    return 0;
  }
  if (mode == "read" && argc > 2) {
    std::ifstream in(argv[2], std::ios::binary);
    if (!in) { std::cout << "ERR cannot open\n"; return 1; }
    for (auto& kv : Read(in)) std::cout << "flag " << kv.first << " = " << kv.second << "\n";
    return 0;
  }
  if (mode == "roundtrip") {
    std::istringstream in(Header());
    const auto got = Read(in);
    // The expected set is written out here, not derived from CarriedSetting (a derived list would shrink with the fault it checks).
    const std::vector<std::pair<std::string, std::string>> want = {{"wwe2k14_match", "type=0;p=343:-1,324:-1;arena=4;pace=1"},
                                                                    {"wwe2k14_match_go", "1"},
                                                                    {"wwe2k14_wm30", "3"},
                                                                    {"wwe2k14_wm30_auto", "false"},
                                                                    {"wwe2k14_scene_victory", "6343"},
                                                                    {"det_frames", "true"}};
    if (got == want && got.size() == 6) { std::cout << "OK " << got.size() << "\n"; return 0; }
    std::cout << "DIFF want " << want.size() << " got " << got.size() << "\n";
    for (auto& kv : want) {
      bool found = false;
      for (auto& g : got) found = found || g == kv;
      if (!found) std::cout << "missing " << kv.first << "\n";
    }
    return 1;
  }
  std::cout << "usage: write FILE | read FILE | roundtrip\n";
  return 2;
}
