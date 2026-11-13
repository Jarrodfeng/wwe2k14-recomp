// Logging, and the one way the renderer reports something that must never happen.
// A problem has a name. Dev builds stop on it, by name. Player builds count it per name and log the first of each; reports read the counts.
#pragma once
#include <cstdint>

namespace rdr {

using LogSink = void (*)(const char* line);
void SetLogSink(LogSink sink);  // null: stderr

void Log(const char* fmt, ...);

// `name` must be a string literal: it is kept by pointer.
void Problem(const char* name, const char* fmt, ...);

struct ProblemCount {
  const char* name;
  uint64_t count;
};
// Fills up to max entries, returns how many there are.
int ProblemCounts(ProblemCount* out, int max);

}  // namespace rdr
