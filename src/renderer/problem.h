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

// S108 AL1 (docs\research\s108\AL1_plan.md): the ONE report of "we could not draw what the game asked". `kind` = a string literal naming WHAT fell short
// (pose_rows, row_bound, model_part, texture_pac, texture_upload, mesh_upload); `object` = what a person needs to find the thing (copied; with `kind` the once-per-run key);
// asked / given = the counts; fmt... = detail (model address, pool, why), NOT part of the key. Writes
//   [rdr][PROBLEM] draw_shortfall: <kind>: <object>: asked <A>, given <G>[: <detail>]
// once per (kind, object) per run (its own set: not Problem's first-of-name gate), in EVERY build, and NEVER aborts (the RDR_DEV rigs replay old records whose
// shortfalls are there on purpose; the line itself is the alarm: triage / bench_line / run_read / the launcher's report fail on [rdr][PROBLEM]). Each new key bumps
// ProblemCounts under `draw_shortfall`. The first kMaxShortfallLines keys are named, the next one prints one limit line, the rest are counted (DrawShortfallTally).
// Call it only when a shortfall was found (the caller tests its own condition first): it takes a lock and a set lookup.
void DrawShortfall(const char* kind, const char* object, uint64_t asked, uint64_t given, const char* fmt = nullptr, ...);
// S108 AL1: a plain once-per-(kind, object) line that is NOT a finding (a fact the code cannot tell from the game's own choice, e.g. a texture name the game itself
// leaves unbound): the formatted text is written as is, at most kMaxNoteLines a run (then one limit line); every new key is counted in the tally.
void DrawNote(const char* kind, const char* object, const char* fmt, ...);
constexpr int kMaxShortfallLines = 64;
constexpr int kMaxNoteLines = 64;
constexpr int kMaxShortfallKeys = 4096;   // keys kept (shortfalls and notes each); past it a call is counted in `unkeyed`, never logged
struct ShortfallTally {
  uint64_t distinct = 0, named = 0, unkeyed = 0;           // DrawShortfall: keys seen, lines named, calls past the key store
  uint64_t notes = 0, notes_named = 0, notes_unkeyed = 0;  // DrawNote: the same
};
ShortfallTally DrawShortfallTally();

}  // namespace rdr
