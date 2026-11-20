// S105 C0b: the test driver for src\launcher\report_findings.h (the C++ port of triage.py).
//   findings_test OUT_FILE SOURCE FILE [FILE...]   -> writes report_findings::Build(FILES, SOURCE) to OUT_FILE
// tools\report\findings_compare.py builds this, runs it on the same logs triage.py reads, and compares the two texts.
// entry: report_findings::Build
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../../src/launcher/report_findings.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: findings_test OUT_FILE SOURCE FILE...\n");
    return 2;
  }
  std::vector<std::filesystem::path> files;
  for (int i = 3; i < argc; ++i) files.emplace_back(argv[i]);
  const std::string text = report_findings::Build(files, argv[2]);
  std::ofstream out(argv[1], std::ios::binary);
  out.write(text.data(), std::streamsize(text.size()));
  return out ? 0 : 2;
}
