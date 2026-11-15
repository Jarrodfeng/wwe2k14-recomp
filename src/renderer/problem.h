// Logging, and the one way the renderer reports something that must never happen.
// A problem has a name. Dev builds stop on it, by name. Player builds count it per name and log the first of each; reports read the counts.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr {

// S102 L1b (selftest only: rplay --light-cube-selftest): a Problem is logged and counted but does not abort a RDR_DEV build.
inline std::atomic<bool> g_problem_nonfatal{false};

using LogSink = void (*)(const char* line);
void SetLogSink(LogSink sink);  // null: stderr

void Log(const char* fmt, ...);

// `name` must be a string literal: it is kept by pointer.
void Problem(const char* name, const char* fmt, ...);

struct ProblemCount {
  const char* name;
  uint64_t count;
};
constexpr int kMaxProblemNames = 128;  // S102 U1: the table size (problem.cpp); a reader needs this many entries to see every name
// Fills up to max entries, returns how many there are.
int ProblemCounts(ProblemCount* out, int max);

}  // namespace rdr
