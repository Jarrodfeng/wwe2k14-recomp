// S105 A10 offline test of src/thread_cpu.h (the [perf][threads] line, the launcher's "threads:" line and the report's Threads section).
// entry: wwe2k14::thread_cpu::Meter::Step / Format / ParseLine / Section (the path perf_mem.h Emit and share_report.h Build run, in that order:
//        two readings -> Compute -> Format -> the log line -> ParseLine of that line -> Summarize -> Section)
// Pure cases on synthetic /proc text (no game, no load: nothing here spins), the planted twins, and one live reading of this process whose own
// sleeping thread must read under 1 %. Build + run: tools\tests\thread_cpu_test.ps1.
#include "thread_cpu.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <tuple>

#if defined(_WIN32)
#include "thread_cpu_win.h"
#endif

using namespace wwe2k14::thread_cpu;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s: ", #c); std::printf(__VA_ARGS__); std::printf("\n"); } else { std::printf("ok   %s\n", #c); } } while (0)

static std::string StatText(int tid, const char* comm, unsigned long long utime, unsigned long long stime) {
  char b[512];
  std::snprintf(b, sizeof(b), "%d (%s) S 1 1 0 0 -1 4194560 100 0 5 0 %llu %llu 0 0 20 0 60 0 1234 100000 200 18446744073709551615 0 0\n", tid, comm,
                utime, stime);
  return b;
}

static Snapshot Snap(double hz, std::initializer_list<std::tuple<int, const char*, unsigned long long, long long>> ts) {
  Snapshot s;
  s.hz = hz;
  for (const auto& [tid, name, ticks, ctx] : ts) {
    Raw r;
    r.name = name, r.ticks = ticks, r.ctx = ctx;
    s.t[tid] = r;
  }
  return s;
}

int main() {
  // 1. ParseStat: a comm with spaces and a ')' (the parse starts after the LAST ')').
  {
    std::string name;
    uint64_t ticks = 0;
    CHECK(ParseStat(StatText(29210, "SDLThread", 700, 300).c_str(), name, ticks) && name == "SDLThread" && ticks == 1000, "ticks=%llu", (unsigned long long)ticks);
    CHECK(ParseStat(StatText(7, "Owner timing (F) x", 40, 2).c_str(), name, ticks) && name == "Owner timing (F) x" && ticks == 42, "name=%s", name.c_str());
    // Planted twin: a parser that stops at the FIRST ')' reads the wrong fields from this text; ours must still give utime + stime.
    CHECK(ParseStat(StatText(8, "a) S 9 (b", 5, 5).c_str(), name, ticks) && ticks == 10, "name=%s ticks=%llu", name.c_str(), (unsigned long long)ticks);
    CHECK(!ParseStat("garbage", name, ticks), "garbage must not parse");
    CHECK(!ParseStat("12 (x)", name, ticks), "a cut line must not parse");
  }
  // 2. ParseStatus: voluntary + nonvoluntary (the one is a suffix of the other).
  {
    int64_t ctx = -1;
    const char* status = "Name:\tSDLThread\nvoluntary_ctxt_switches:\t1000\nnonvoluntary_ctxt_switches:\t24\n";
    CHECK(ParseStatus(status, ctx) && ctx == 1024, "ctx=%lld", (long long)ctx);
    CHECK(!ParseStatus("Name:\tx\n", ctx), "no switch lines");
  }
  // 2b. S105 A10 review F1: a real /proc status file is ~1.3 KB and the switch counters are its LAST lines. One short read (the first build:
  // fread into 1024 bytes) never saw them. ReadProcFile reads to EOF.
  {
    std::string status = "Name:\tSDLThread\nUmask:\t0077\nState:\tS (sleeping)\nTgid:\t29000\nPid:\t29210\n";
    while (status.size() < 1200) status += "Groups:\t1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30\n";
    status += "voluntary_ctxt_switches:\t1000\nnonvoluntary_ctxt_switches:\t24\n";
    CHECK(status.size() > 1100, "status is %zu bytes", status.size());
    char shortbuf[1024];
    std::snprintf(shortbuf, sizeof(shortbuf), "%s", status.c_str());
    int64_t cx = -1;
    CHECK(!ParseStatus(shortbuf, cx), "planted fault: a 1 KB cut loses the counters (the old read)");
    const char* path = "thread_cpu_test_status.tmp";
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(status.data(), 1, status.size(), f); std::fclose(f); }
    char buf[4096];
    const size_t n = ReadProcFile(path, buf, sizeof(buf));
    CHECK(n == status.size() && ParseStatus(buf, cx) && cx == 1024, "ReadProcFile n=%zu ctx=%lld", n, (long long)cx);
    std::remove(path);
    CHECK(ReadProcFile("no_such_file.tmp", buf, sizeof(buf)) == 0, "missing file");
  }
  // 2c. review F5: a reused tid (same name, new start time) has no baseline.
  {
    std::string name;
    uint64_t ticks = 0, start = 0;
    CHECK(ParseStat(StatText(1, "x", 1, 2).c_str(), name, ticks, &start) && start == 1234, "start=%llu", (unsigned long long)start);
    Snapshot p = Snap(100.0, {{1, "SDLThread", 1000, 0}}), c = Snap(100.0, {{1, "SDLThread", 9000, 0}});
    p.t[1].start = 10, c.t[1].start = 10;
    CHECK(Compute(p, c, 10.0, 1.0).top.size() == 1, "same thread");
    c.t[1].start = 99;
    CHECK(Compute(p, c, 10.0, 1.0).top.empty(), "a reused tid is a new thread: no baseline this window");
  }
  // 3. Compute + Format: the planted twin is a thread with +10 s x hz of CPU in a 10 s window: it must print 100 % and be flagged SPINNING.
  const double hz = 100.0;
  const Snapshot a = Snap(hz, {{1, "SDLThread", 1000, 100}, {2, "idle", 50, 10}, {3, "poller", 10, 100}, {4, "leaves", 5, 5}, {6, "XThreadE67A8480", 0, 0}});
  const Snapshot b = Snap(hz, {{1, "SDLThread", 2000, 2100}, {2, "idle", 52, 14}, {3, "poller", 20, 10100}, {5, "newcomer", 900, 3}, {6, "XThreadE67A8480", 520, 12}});
  const Window w = Compute(a, b, 10.0, 22.2);
  CHECK(w.top.size() == 3, "top=%zu", w.top.size());
  CHECK(w.top.size() == 3 && w.top[0].name == "SDLThread" && w.top[0].tid == 1 && std::abs(w.top[0].pct - 100.0) < 1e-9, "planted spinner pct=%.2f", w.top.empty() ? -1.0 : w.top[0].pct);
  CHECK(w.top.size() == 3 && std::abs(w.top[0].wakes - 200.0) < 1e-9, "wakes=%.1f", w.top.empty() ? -1.0 : w.top[0].wakes);
  CHECK(w.top.size() == 3 && w.top[1].name == "XThreadE67A8480" && std::abs(w.top[1].pct - 52.0) < 1e-9, "second");
  // the 1 ms poller: 1 % CPU but 1000 wakes/s stays listed; "idle" (0.2 %, 0.4 wakes/s) is summed into the rest; a new thread has no baseline.
  CHECK(w.top.size() == 3 && w.top[2].name == "poller" && std::abs(w.top[2].wakes - 1000.0) < 1e-9, "poller kept for its wakes");
  CHECK(w.rest_n == 1 && std::abs(w.rest_pct - 0.2) < 1e-9, "rest_n=%d rest=%.3f", w.rest_n, w.rest_pct);
  CHECK(std::abs(w.proc - 153.2) < 1e-6 && w.threads == 5, "proc=%.3f threads=%d", w.proc, w.threads);
  const std::string text = Format(w);
  std::printf("%s\n", text.c_str());
  CHECK(text.find("SDLThread#1 100.0% w=200/s") != std::string::npos, "format");
  // The idle twin must NOT be called a spinner, and Meter's first call is only the baseline.
  {
    Meter m;
    Window mw;
    CHECK(!m.Step(a, 100.0, 12.2, mw), "first call = baseline only");
    CHECK(m.Step(b, 110.0, 22.2, mw) && mw.top.size() == 3, "second call gives the window");
  }
  // 4. The report side: ParseLine reads Format's own text back (both markers), Summarize flags 2 windows in a row at >= 90 %, Section prints it.
  {
    Window back;
    CHECK(ParseLine(std::string("12:00:01.123 [wwe2k14][perf][threads] ") + text, "[wwe2k14][perf][threads] ", back), "parse back");
    CHECK(back.top.size() == 3 && back.top[0].name == "SDLThread" && back.top[0].tid == 1 && std::abs(back.top[0].pct - 100.0) < 0.05 &&
              std::abs(back.top[0].wakes - 200.0) < 0.5 && back.rest_n == 1 && back.threads == 5,
          "parsed fields");
    CHECK(!ParseLine("[wwe2k14][perf][mem] t=1 s", "[wwe2k14][perf][threads] ", back), "another line is not a threads line");
    Window with_comma;  // a comm with spaces and parentheses must survive the round trip
    const Snapshot c = Snap(hz, {{9, "Owner timing (F", 0, 0}});
    const Snapshot d = Snap(hz, {{9, "Owner timing (F", 400, 50}});
    with_comma = Compute(c, d, 10.0, 1.0);
    Window rt;
    CHECK(ParseLine("  48.260 threads: " + Format(with_comma), "threads: ", rt) && rt.top.size() == 1 && rt.top[0].name == "Owner timing (F" && rt.top[0].tid == 9,
          "launcher line round trip");

    std::vector<Window> spin2 = {back, back}, spin1 = {back}, once_then_idle;
    spin2[1].t = 32.2;
    CHECK(Summarize(spin2)[0].spinning && Summarize(spin2)[0].key == "SDLThread#1", "planted twin: 100 %% twice in a row is SPINNING");
    CHECK(!Summarize(spin1)[0].spinning, "one hot window is not");
    Window idle_w = back;
    idle_w.top[0].pct = 3.0;
    once_then_idle = {back, idle_w, back};  // hot, cool, hot: never two in a row
    CHECK(!Summarize(once_then_idle)[0].spinning, "hot / cool / hot is not SPINNING");
    Window idle_only = back;
    for (Row& r : idle_only.top) r.pct = 2.0;
    CHECK(!Summarize({idle_only, idle_only})[0].spinning, "an idle twin (2 %%) is not flagged");
    const std::string sec = Section(spin2, {});
    std::printf("%s", sec.c_str());
    CHECK(sec.find("SPINNING (game): SDLThread#1") != std::string::npos, "section names the spinner");
    CHECK(sec.find("launcher: no thread lines in this log") != std::string::npos, "an empty log says so");
    CHECK(Section({idle_only, idle_only}, {}).find("SPINNING") == std::string::npos, "no SPINNING for the idle twin");
    CHECK(sec.find("most wakes: poller#3 at 1000/s") != std::string::npos, "the polled-wait thread is named");
  }
  // 5. Live: this process's own readings. A thread that only sleeps must read under 1 % (and nothing here spins).
  {
#if defined(_WIN32)
    Meter m;
    Window lw;
    std::atomic<int32_t> sleeper_tid{0};
    std::atomic<bool> stop{false};
    std::thread sleeper([&] {
      sleeper_tid = int32_t(GetCurrentThreadId());
      while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    StepNow(m, 0, lw);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const bool got = StepNow(m, 0.6, lw);
    stop = true;
    sleeper.join();
    double pct = -1;
    for (const Row& r : lw.top) if (r.tid == sleeper_tid.load()) pct = r.pct;
    CHECK(got && lw.threads >= 2, "live window threads=%d", lw.threads);
    CHECK(pct < 1.0, "the sleeping thread reads %.2f %% (not listed = under 1 %%)", pct);
    CHECK(lw.proc < 100.0, "proc=%.1f%%", lw.proc);
#else
    const Snapshot live = TakeSnapshot();
    CHECK(!live.t.empty(), "this process has threads in /proc");
#endif
  }
  std::printf(g_fail ? "FAILED %d\n" : "all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
