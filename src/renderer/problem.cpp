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

constexpr int kMaxNames = kMaxProblemNames;
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
  if (stop && !g_problem_nonfatal.load(std::memory_order_relaxed)) {
    std::fflush(nullptr);  // S102 D1 (review F4): the lines before the stop (a lost device's dump) reach the file / pipe
    std::abort();
  }
}

// ---- S108 AL1: DrawShortfall / DrawNote (problem.h) -------------------------------------------------------------------------------------------------------------
// (Its headers are included here, not at the top: Problem's line literal must stay at line 72, the writer that tools\report\triage.py's pattern table names and its selftest lints.)
}  // namespace rdr
#include <mutex>
#include <set>
#include <string>
namespace rdr {
namespace {
struct ShortfallStore {
  std::mutex mu;
  std::set<std::string> keys, note_keys;
  ShortfallTally tally;
  bool limit_said = false, note_limit_said = false;
};
ShortfallStore& Store() {
  // Made once (thread-safe) and never destroyed: a render or guest thread may still report while the process exits, after the static destructors ran (a destroyed set there hung or
  // crashed the caller: draw_shortfall_unit --late-exit). The memory goes with the process.
  static ShortfallStore* s = new ShortfallStore;
  return *s;
}
// 0 = already seen / not kept; 1 = new, name it; 2 = new, say the limit line; 3 = new, counted only
int Admit(bool note, const char* kind, const char* object) {
  ShortfallStore& s = Store();
  std::string key = std::string(kind ? kind : "?") + '\x1f' + (object ? object : "?");
  std::lock_guard<std::mutex> lk(s.mu);
  std::set<std::string>& keys = note ? s.note_keys : s.keys;
  if (keys.count(key)) return 0;
  if (keys.size() >= size_t(kMaxShortfallKeys)) {
    ++(note ? s.tally.notes_unkeyed : s.tally.unkeyed);
    return 0;
  }
  keys.insert(std::move(key));
  uint64_t& distinct = note ? s.tally.notes : s.tally.distinct;
  uint64_t& named = note ? s.tally.notes_named : s.tally.named;
  bool& said = note ? s.note_limit_said : s.limit_said;
  ++distinct;
  if (named < uint64_t(note ? kMaxNoteLines : kMaxShortfallLines)) return ++named, 1;
  if (!said) return said = true, 2;
  return 3;
}
}  // namespace

void DrawShortfall(const char* kind, const char* object, uint64_t asked, uint64_t given, const char* fmt, ...) {
  const int a = Admit(false, kind, object);
  if (a == 0) return;
  Bump("draw_shortfall");
  if (a == 3) return;
  char line[1024];
  if (a == 2) {
    std::snprintf(line, sizeof(line), "[rdr][PROBLEM] draw_shortfall: limit: %d shortfalls named this run; every further one is counted (ProblemCounts draw_shortfall), not named", kMaxShortfallLines);
    Emit(line);
    return;
  }
  char detail[640] = "";
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);
  }
  std::snprintf(line, sizeof(line), "[rdr][PROBLEM] draw_shortfall: %s: %s: asked %llu, given %llu%s%s", kind ? kind : "?", object ? object : "?", static_cast<unsigned long long>(asked),
                static_cast<unsigned long long>(given), detail[0] ? ": " : "", detail);
  Emit(line);
}

void DrawNote(const char* kind, const char* object, const char* fmt, ...) {
  const int a = Admit(true, kind, object);
  if (a == 0 || a == 3) return;
  char line[1024];
  if (a == 2) {
    std::snprintf(line, sizeof(line), "[rdr][note] limit: %d notes written this run; every further one is counted, not written (the last kind: %s)", kMaxNoteLines, kind ? kind : "?");
    Emit(line);
    return;
  }
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Emit(line);
}

ShortfallTally DrawShortfallTally() {
  ShortfallStore& s = Store();
  std::lock_guard<std::mutex> lk(s.mu);
  return s.tally;
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
