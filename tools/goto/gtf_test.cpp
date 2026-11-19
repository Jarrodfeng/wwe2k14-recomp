// S103 lane GTF: runs the 30 Years jump's call-site filter (src/wm30_args.h IsSelectionReadSite, what the sub_82A9D890 hook in
// src/wm30_jump.h asks) on each stdin line (a hex return address). Built and driven by tools/goto/gtf_selftest.py. No game, no SDK.
// Output, one line per input: "ACT" | "SKIP".
#include <cstdint>
#include <iostream>
#include <string>

#include "wm30_args.h"

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    const uint32_t lr = uint32_t(std::stoul(line, nullptr, 16));
    std::cout << (wwe2k14::wm30::IsSelectionReadSite(lr) ? "ACT" : "SKIP") << "\n";
  }
  return 0;
}
