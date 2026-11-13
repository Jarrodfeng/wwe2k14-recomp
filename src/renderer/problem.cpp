#include "problem.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rdr {
namespace {

LogSink g_sink = nullptr;

void Emit(const char* line) {
  if (g_sink) {
    g_sink(line);
  } else {
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
  }
}

constexpr int kMaxNames = 128;
struct Slot {
  std::atomic<const char*> name{nullptr};
  std::atomic<uint64_t> count{0};
};
Slot g_slots[kMaxNames];

// Returns the count before this one, or ~0 when the table is full.
uint64_t Bump(const char* name) {
  for (Slot& slot : g_slots) {
    const char* current = slot.name.load(std::memory_order_acquire);
    if (!current) {
      const char* expected = nullptr;
      if (slot.name.compare_exchange_strong(expected, name, std::memory_order_acq_rel)) current = name;
      else current = expected;
    }
    if (current == name || std::strcmp(current, name) == 0) return slot.count.fetch_add(1, std::memory_order_relaxed);
  }
  return ~uint64_t(0);
}

}  // namespace

void SetLogSink(LogSink sink) { g_sink = sink; }

void Log(const char* fmt, ...) {
  char line[8192];   // S101 lane 220: was 1024, which cut the props census (content-differs notes) before its "first failed check" text
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Emit(line);
}

void Problem(const char* name, const char* fmt, ...) {
  const uint64_t before = Bump(name);
#ifdef RDR_DEV
  (void)before;
  constexpr bool stop = true;
#else
  constexpr bool stop = false;
  if (before != 0) return;  // player builds log the first of each name only
#endif
  char detail[768];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(detail, sizeof(detail), fmt, args);
  va_end(args);
  char line[1024];
  std::snprintf(line, sizeof(line), "[rdr][PROBLEM] %s: %s", name, detail);
  Emit(line);
  if (stop) std::abort();
}

int ProblemCounts(ProblemCount* out, int max) {
  int n = 0;
  for (Slot& slot : g_slots) {
    const char* name = slot.name.load(std::memory_order_acquire);
    if (!name) break;
    if (n < max) out[n] = {name, slot.count.load(std::memory_order_relaxed)};
    ++n;
  }
  return n;
}

}  // namespace rdr
