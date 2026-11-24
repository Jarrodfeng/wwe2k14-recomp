// draw_shortfall_unit: the offline proof of S108 lane AL1's shared report (src\renderer\problem.h DrawShortfall / DrawNote; docs\research\s108\AL1_plan.md). 0 game runs.
// Built twice by tools\report\build_draw_shortfall_unit.ps1: a player build and a -DRDR_DEV build (the rigs' define). Each line the report writes is captured through rdr::SetLogSink and printed
// as `LINE <text>` (the selftest matches every one against tools\report\triage.py's `problem` regex). Checks (exit 1 when any failed):
//   sf.one_line          one shortfall = one line in the exact format, naming the kind, the object, asked and given and the detail
//   sf.once_per_key      the same (kind, object) again = no line; another kind of the same object = a line
//   sf.each_model_named  two models with the same kind = two lines (Problem's first-of-name gate would name one)          <- --plant problem (player build)
//   sf.no_abort          the dev build returns from the report (Problem aborts an RDR_DEV build)                       <- --plant problem (dev build: the process aborts)
//   sf.cap               70 distinct = 64 named + 1 limit line; the tally says 70 distinct, 64 named; ProblemCounts draw_shortfall = 70
//   sf.threads           8 threads x 500 calls over 50 keys = exactly 50 lines, tally 50
//   sf.notes             DrawNote: a plain line (not [rdr][PROBLEM]) once per key; 70 keys = 64 + 1 limit line; counted apart (notes 70)
//   sf.tally_unkeyed     past kMaxShortfallKeys keys a call is counted in `unkeyed`, never logged
//   sf.late_exit         (`--late-exit`, its own run) a report from an exit handler that runs after the static destructors: still one line per new key, no crash or hang
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer/problem.h"

static std::mutex g_mu;
static std::vector<std::string> g_lines;
static void Sink(const char* line) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_lines.emplace_back(line);
  std::printf("LINE %s\n", line);
}
static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why) {
  std::printf("%s  %-20s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  std::fflush(stdout);
  if (!ok) ++g_fail;
}
static size_t Lines() { std::lock_guard<std::mutex> lk(g_mu); return g_lines.size(); }
static std::string LineAt(size_t i) { std::lock_guard<std::mutex> lk(g_mu); return i < g_lines.size() ? g_lines[i] : std::string(); }
static bool g_plant_problem = false;

// The report under test, or (planted) the old FAN way: rdr::Problem per model.
static void Report(const char* kind, const char* object, uint64_t asked, uint64_t given, const char* detail) {
  if (g_plant_problem) rdr::Problem("weapon_pose_short_rows", "%s: asked %llu, given %llu: %s", object, (unsigned long long)asked, (unsigned long long)given, detail);
  else rdr::DrawShortfall(kind, object, asked, given, "%s", detail);
}

static uint64_t ProblemCount(const char* name) {
  rdr::ProblemCount c[rdr::kMaxProblemNames];
  const int n = rdr::ProblemCounts(c, rdr::kMaxProblemNames);
  for (int i = 0; i < n && i < rdr::kMaxProblemNames; ++i)
    if (std::strcmp(c[i].name, name) == 0) return c[i].count;
  return 0;
}

// ---- sf.late_exit (`--late-exit`, review fix): a report made AFTER the report's own store would be destroyed at exit (a render or guest thread still drawing while the process exits). The handler is
// registered before the store's first use, so it runs after every static destroyed later than it. It must still name each new key once and keep the old keys (no crash, no hang: the selftest
// gives it 30 s). The counter is a plain int: the vector sink above may be gone by then.
static int g_late_lines = 0;
static void LateSink(const char* line) { ++g_late_lines, std::printf("LINE %s\n", line), std::fflush(stdout); }
static void LateExit() {
  const int a = g_late_lines;
  for (int i = 0; i < 20; ++i) rdr::DrawShortfall("pose_rows", ("late model " + std::to_string(i)).c_str(), 2, 1, "at exit");
  for (int i = 0; i < 3; ++i) rdr::DrawShortfall("pose_rows", ("early model " + std::to_string(i)).c_str(), 2, 1, "again at exit");
  const rdr::ShortfallTally t = rdr::DrawShortfallTally();
  const bool ok = g_late_lines - a == 20 && t.distinct == 23;
  std::printf("%s  %-20s late lines %d (want 20), tally distinct %llu (want 23)\n", ok ? "PASS" : "FAIL", "sf.late_exit", g_late_lines - a, static_cast<unsigned long long>(t.distinct));
  std::fflush(stdout);
  if (!ok) std::_Exit(1);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "problem") == 0) g_plant_problem = true;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--late-exit") == 0) {
      rdr::SetLogSink(LateSink);
      std::atexit(LateExit);   // before the store's first use (the next line)
      for (int k = 0; k < 3; ++k) rdr::DrawShortfall("pose_rows", ("early model " + std::to_string(k)).c_str(), 2, 1, "in main");
      return 0;
    }
  rdr::SetLogSink(Sink);
#ifdef RDR_DEV
  std::printf("build: dev (RDR_DEV)\n");
#else
  std::printf("build: player\n");
#endif
  // ---- sf.one_line / sf.no_abort ----
  {
    const size_t a = Lines();
    Report("pose_rows", "event prop 2591 entry 4", 658, 96, "model 0xA0001000, pool weapon:ev2591/4: the vertices past row 95 stay at their file position");
    Check(true, "sf.no_abort", "the report returned");   // (the planted dev build never gets here: Problem aborts it)
    const std::string want = "[rdr][PROBLEM] draw_shortfall: pose_rows: event prop 2591 entry 4: asked 658, given 96: model 0xA0001000, pool weapon:ev2591/4: the vertices past row 95 stay at their file position";
    Check(Lines() == a + 1 && LineAt(a) == want, "sf.one_line", "got '" + LineAt(a) + "'");
  }
  // ---- sf.each_model_named: a second model of the same kind ----
  {
    const size_t a = Lines();
    Report("pose_rows", "event prop 70 entry 0", 731, 96, "model 0xA0002000");
    Check(Lines() == a + 1 && LineAt(a).find("event prop 70 entry 0") != std::string::npos, "sf.each_model_named", "lines " + std::to_string(Lines() - a) + " for the second model");
  }
  if (g_plant_problem) {
    std::printf("planted: problem (the rest is not run)\n");
    return g_fail ? 1 : 0;
  }
  // ---- sf.once_per_key ----
  {
    const size_t a = Lines();
    for (int i = 0; i < 5; ++i) rdr::DrawShortfall("pose_rows", "event prop 2591 entry 4", 658, 96, "again");
    const size_t same = Lines() - a;
    rdr::DrawShortfall("model_part", "event prop 2591 entry 4", 1, 0, "another kind");
    Check(same == 0 && Lines() - a == 1, "sf.once_per_key", "same key again " + std::to_string(same) + " line(s), another kind " + std::to_string(Lines() - a - same));
  }
  // ---- sf.threads: 50 keys, each asked 80 times from 8 threads ----
  {
    const size_t a = Lines();
    const rdr::ShortfallTally t0 = rdr::DrawShortfallTally();
    std::vector<std::thread> th;
    for (int t = 0; t < 8; ++t)
      th.emplace_back([t] {
        for (int i = 0; i < 500; ++i) {
          const std::string obj = "thread model " + std::to_string((i * 7 + t) % 50);
          rdr::DrawShortfall("mesh_upload", obj.c_str(), 1, 0, "thread %d", t);
        }
      });
    for (std::thread& x : th) x.join();
    const rdr::ShortfallTally t1 = rdr::DrawShortfallTally();
    Check(Lines() - a == 50 && t1.distinct - t0.distinct == 50, "sf.threads", "lines " + std::to_string(Lines() - a) + ", new keys " + std::to_string(t1.distinct - t0.distinct) + " (want 50 / 50)");
  }
  // ---- sf.cap: so far 1 + 1 + 1 + 50 = 53 named; 17 more distinct make 70 in all: 64 named, then one limit line, then counted only ----
  {
    const size_t a = Lines();
    for (int i = 0; i < 17; ++i) rdr::DrawShortfall("texture_upload", ("flood model " + std::to_string(i)).c_str(), 1, 0, "flood");
    const rdr::ShortfallTally t = rdr::DrawShortfallTally();
    size_t limit = 0, named = 0;
    for (size_t i = a; i < Lines(); ++i) (LineAt(i).find("draw_shortfall: limit:") != std::string::npos ? limit : named) += 1;
    Check(named == 11 && limit == 1 && t.distinct == 70 && t.named == 64 && ProblemCount("draw_shortfall") == 70, "sf.cap",
          "named " + std::to_string(named) + " + limit " + std::to_string(limit) + " (want 11 + 1); tally distinct " + std::to_string(t.distinct) + " named " + std::to_string(t.named) + ", ProblemCounts " + std::to_string(ProblemCount("draw_shortfall")));
  }
  // ---- sf.notes ----
  {
    const size_t a = Lines();
    for (int i = 0; i < 70; ++i) rdr::DrawNote("texture_not_found", ("arena 5|texture 'n" + std::to_string(i) + "' not found").c_str(), "[wwe2k14][model_load] arena 5: texture 'n%d' not found", i);
    rdr::DrawNote("texture_not_found", "arena 5|texture 'n0' not found", "again");
    const rdr::ShortfallTally t = rdr::DrawShortfallTally();
    size_t problem = 0, limit = 0;
    for (size_t i = a; i < Lines(); ++i) problem += LineAt(i).find("[rdr][PROBLEM]") != std::string::npos, limit += LineAt(i).find("[rdr][note] limit:") != std::string::npos;
    Check(Lines() - a == 65 && limit == 1 && problem == 0 && t.notes == 70 && t.notes_named == 64 && t.distinct == 70, "sf.notes",
          "lines " + std::to_string(Lines() - a) + " (want 64 + 1 limit), PROBLEM lines " + std::to_string(problem) + ", notes " + std::to_string(t.notes) + " written " + std::to_string(t.notes_named));
  }
  // ---- sf.tally_unkeyed: fill the key store ----
  {
    const size_t a = Lines();
    for (int i = 0; i < rdr::kMaxShortfallKeys; ++i) rdr::DrawShortfall("row_bound", ("store model " + std::to_string(i)).c_str(), 2000, 0, nullptr);
    const rdr::ShortfallTally t = rdr::DrawShortfallTally();
    Check(Lines() == a && t.distinct == uint64_t(rdr::kMaxShortfallKeys) && t.unkeyed == 70, "sf.tally_unkeyed",
          "lines " + std::to_string(Lines() - a) + ", distinct " + std::to_string(t.distinct) + ", unkeyed " + std::to_string(t.unkeyed) + " (want 0 / 4096 / 70)");
  }
  std::printf("%s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
