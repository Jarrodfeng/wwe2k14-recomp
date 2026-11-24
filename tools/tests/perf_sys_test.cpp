// Session 101 lane 302 offline test of src/perf_sys.h (the [perf][sys] line). Windows only (the game's PC build).
// Checks: the pure Derive / Format on made-up counters (exact numbers), a missing field is left out, and a live two-reading window on this
// machine (sys_cpu / proc_cpu in range while a busy thread runs, proc_cpu rises with the load, free_mb > 0, the hard-fault struct passes its
// self-check or is left out). Build + run: tools\tests\perf_sys_test.ps1.
#include "perf_sys.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace wwe2k14::perf_sys;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s: ", #c); std::printf(__VA_ARGS__); std::printf("\n"); } else { std::printf("ok   %s\n", #c); } } while (0)

// S102 S0-2: `perf_sys_test.exe --seconds N [--out FILE]`: the standalone sampler. Runs N seconds with one named and one unnamed busy thread of this
// process, takes one [perf][sys] window per second (the same Take / Format the game uses) and prints / appends each line. tools\perf\thread_busy.ps1
// -ProcessName perf_sys_test samples this process's threads at the same time.
static int RunSampler(int seconds, const char* out_path) {
  std::atomic<bool> stop{false};
  auto burn = [&] { volatile double x = 0; while (!stop.load()) x += 1.0; };
  std::thread named([&] {
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"heat_burn_named");
#endif
    burn();
  });
  std::thread unnamed(burn);
  FILE* f = out_path ? std::fopen(out_path, "w") : nullptr;
  Sample s;
  Take(s, 0, 0, 0);  // primes
  for (int i = 0; i < seconds; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!Take(s, 0, 0, 0)) continue;
    const std::string line = Format(s, double(i + 1), false);
    std::printf("%s\n", line.c_str());
    if (f) std::fprintf(f, "%s\n", line.c_str());
  }
  stop = true;
  named.join();
  unnamed.join();
  if (f) std::fclose(f);
  return 0;
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--seconds") {
      const char* out = nullptr;
      for (int j = 1; j + 1 < argc; ++j) if (std::string(argv[j]) == "--out") out = argv[j + 1];
      return RunSampler(std::atoi(argv[i + 1]), out);
    }
  }
  // Pure math: 10 s window, 4 cores. 100 ns ticks: 40 cores-seconds of total = 10 s * 4 * 1e7.
  Raw a, b;
  a.ok = b.ok = true;
  a.at = std::chrono::steady_clock::now();
  b.at = a.at + std::chrono::seconds(10);
  a.sys_total = 0, b.sys_total = 4ull * 10 * 10000000ull;
  a.sys_busy = 0, b.sys_busy = b.sys_total / 4;  // 25 % busy
  a.proc_ticks = 0, b.proc_ticks = b.sys_total / 10;  // 10 % of the whole machine
  a.proc_faults = 1000, b.proc_faults = 11000;
  a.page_reads = 50, b.page_reads = 150;
  b.free_mb = 1234, b.commit_mb = 2000, b.commit_limit_mb = 9000, b.cores = 4;
  Sample s = Derive(a, b, 1500.0, 0.05, 7, 12.5);
  CHECK(s.win_s > 9.99 && s.win_s < 10.01, "win=%f", s.win_s);
  CHECK(s.sys_cpu > 24.99 && s.sys_cpu < 25.01, "sys_cpu=%f", s.sys_cpu);
  CHECK(s.proc_cpu > 9.99 && s.proc_cpu < 10.01, "proc_cpu=%f", s.proc_cpu);
  CHECK(s.proc_faults_s > 999.9 && s.proc_faults_s < 1000.1, "proc_faults_s=%f", s.proc_faults_s);
  CHECK(s.hard_faults_s > 9.99 && s.hard_faults_s < 10.01, "hard_faults_s=%f", s.hard_faults_s);
  const std::string line = Format(s, 30.0, false);
  std::printf("%s\n", line.c_str());
  CHECK(line.find("sys_cpu=25") != std::string::npos && line.find("proc_cpu=10") != std::string::npos, "format");
  CHECK(line.find("log_write_ms=1500.00") != std::string::npos && line.find("log_dropped=7") != std::string::npos && line.find("file_write_ms=12.50") != std::string::npos, "format log fields");
  // A counter the platform cannot read (page reads) is left out, never 0.
  a.page_reads = b.page_reads = ~0ull;
  CHECK(Format(Derive(a, b, -1, -1, -1, -1), 1.0, true).find("hard_faults_s") == std::string::npos, "unreadable field left out");
  CHECK(Format(Derive(a, b, -1, -1, -1, -1), 1.0, true).find("log_write_ms") == std::string::npos, "unknown log fields left out");
  // S102 S0-2: heat fields. A platform that does not probe prints none; one that probes prints n/a (never 0) for a counter it lacks.
  CHECK(Format(Derive(a, b, -1, -1, -1, -1), 1.0, true).find("perf_pct") == std::string::npos, "no heat probe: no heat fields");
  {
    Sample h = Derive(a, b, -1, -1, -1, -1);
    h.heat.probed = true;
    h.heat.perf_pct = 131.4, h.heat.perf_limit = 88, h.heat.limit_flags = 2;  // package watts unreadable
    const std::string hl = Format(h, 1.0, false);
    std::printf("%s\n", hl.c_str());
    CHECK(hl.find("perf_pct=131") != std::string::npos && hl.find("perf_limit=88") != std::string::npos && hl.find("limit_flags=2") != std::string::npos, "heat fields");
    CHECK(hl.find("pkg_w=n/a") != std::string::npos, "unreadable package watts says n/a");
    CHECK(hl.find("pkg_w=0") == std::string::npos, "never a 0 for an unreadable counter");
    h.heat.pkg_w = 12.64;
    CHECK(Format(h, 1.0, false).find("pkg_w=12.6") != std::string::npos, "package watts when readable");
  }

  // Live: prime, burn one core for ~1 s, read the window.
  Sample live;
  CHECK(!Take(live, 0, 0, 0), "the first call only primes");
  wwe2k14::perf_sys::NoteFileWrite(42.0);
  wwe2k14::perf_sys::NoteFileWrite(7.0);
  std::atomic<bool> stop{false};
  std::thread burn([&] { volatile double x = 0; while (!stop.load()) x += 1.0; });
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  const bool ok = Take(live, 0, 0, 0);
  stop = true;
  burn.join();
  CHECK(ok, "second call yields a window");
  const std::string lv = Format(live, 1.0, false);
  std::printf("live  %s\n", lv.c_str());
  CHECK(live.sys_cpu >= 0 && live.sys_cpu <= 100, "sys_cpu=%f", live.sys_cpu);
  CHECK(live.proc_cpu > 0 && live.proc_cpu <= 100, "proc_cpu=%f", live.proc_cpu);
  CHECK(live.cores > 0, "cores=%d", live.cores);
  CHECK(live.free_mb > 0, "free_mb=%f", live.free_mb);
  CHECK(live.file_write_ms > 41.9 && live.file_write_ms < 42.1, "file_write_ms=%f (the note must come through once)", live.file_write_ms);
  CHECK(live.heat.probed, "heat is probed on Windows");
  std::printf("heat: perf_pct=%.0f perf_limit=%.0f limit_flags=%.0f pkg_w=%.1f (-1 = n/a on this machine)\n", live.heat.perf_pct, live.heat.perf_limit,
              live.heat.limit_flags, live.heat.pkg_w);
  CHECK(live.heat.perf_pct > 0 && live.heat.perf_pct < 1000, "real performance %% on this machine, got %f", live.heat.perf_pct);
  std::printf(live.hard_faults_s >= 0 ? "hard-fault struct: trusted (self-check passed), %.0f /s\n" : "hard-fault struct: dropped by its self-check\n", live.hard_faults_s);
  {  // S102 S0-2 review item 8: the wwe2k14_perf_heat escape hatch (perf_heat::Enabled) is read per window; off = no heat fields at all
    wwe2k14::perf_heat::Enabled().store(false);
    Sample off;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool ok_off = Take(off, 0, 0, 0);
    CHECK(ok_off && !off.heat.probed && Format(off, 1.0, false).find("perf_pct") == std::string::npos, "heat switch off: no heat fields, no PDH");
    wwe2k14::perf_heat::Enabled().store(true);
  }
  std::printf(g_fail ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
