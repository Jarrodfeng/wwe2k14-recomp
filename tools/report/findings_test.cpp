// S105 C0b: the test driver for src\launcher\report_findings.h (the C++ port of triage.py).
//   findings_test OUT_FILE SOURCE FILE [FILE...]   -> writes report_findings::Build(FILES, SOURCE) to OUT_FILE
// tools\report\findings_compare.py builds this, runs it and triage.py on the same logs, and compares the two texts.
// S108 RPT: with FINDINGS_PEAK=1 in the environment it also prints `peak_working_set_bytes N` on stderr (Windows: the process's peak working set),
// so tools\report\facts_selftest.py can prove a 200 MB log is read in bounded memory.
// entry: report_findings::Build
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../../src/launcher/report_findings.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

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
#ifdef _WIN32
  char peak[4] = {0};
  if (GetEnvironmentVariableA("FINDINGS_PEAK", peak, sizeof peak) && peak[0] == '1') {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) std::fprintf(stderr, "peak_working_set_bytes %llu\n", (unsigned long long)pmc.PeakWorkingSetSize);
  }
#endif
  return out ? 0 : 2;
}
