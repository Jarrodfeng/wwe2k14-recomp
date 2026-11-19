#pragma once
// S102 CEN1: the census flush. A lever measured by the in-run flip A/B (docs\renderer\IN_RUN_AB.md) logs its census line only every 600 frames / runs, so a flip run
// (a phase = 60 frames) showed no census after the first frame, and "not separated" could not be told from "never took" (present_fetch, record_skip UQ1 / UQ2).
// One generic mechanism: when ANY PerFrame switch's seen value changes (rdr_switches.h Seen) or the run ends, Raise() bumps a generation counter and logs ONE marker line
//   [census] flush gen G phase end: NAME=V        (V = the value the switch HAD: the phase that just ended; NAME "run" = the run's end)
// Every census site keeps a `Site` (its last-seen generation) and asks `Due(site)` beside its own 600-frame test: when the generation moved it prints its line NOW, so the line
// follows the marker and belongs to that ended phase (tools\surface\flip_line.py reads it that way). No per-site label, no strings per draw: one relaxed load per census call site,
// which already runs once per frame / per run. Pure (no game includes); the unit test is census_flush_test.cpp; the lint is tools\renderer\census_flush_lint.py.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace census_flush {

using LogFn = void (*)(const char*);
inline std::atomic<uint32_t> g_gen{0};

// The phase that just ended: switch NAME and the value it HAD, or name "run" at the run's end. `log` may be null (the generation still moves).
inline void Raise(const char* name, int64_t old_value, LogFn log) {
  const uint32_t gen = g_gen.fetch_add(1, std::memory_order_release) + 1;
  if (!log) return;
  char line[200];
  if (name && std::strcmp(name, "run") == 0) std::snprintf(line, sizeof(line), "[census] flush gen %u phase end: run end", gen);
  else std::snprintf(line, sizeof(line), "[census] flush gen %u phase end: %s=%lld", gen, name ? name : "?", (long long)old_value);
  log(line);
}
inline uint32_t Generation() { return g_gen.load(std::memory_order_relaxed); }

// One per census site (a member / static of the thread that prints). Starts at 0: no flush before the first Raise.
struct Site {
  std::atomic<uint32_t> last{0};
};
// True once per generation per site. The common path is a single relaxed load and a compare.
inline bool Due(Site& s) {
  const uint32_t g = g_gen.load(std::memory_order_relaxed);
  if (g == s.last.load(std::memory_order_relaxed)) return false;
  s.last.store(g, std::memory_order_relaxed);
  return true;
}

}  // namespace census_flush
