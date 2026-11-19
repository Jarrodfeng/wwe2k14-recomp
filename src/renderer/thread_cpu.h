// S102 S0-3: the calling thread's CPU time over a window, with no windows.h in it (live.cpp includes this; windows.h and the renderer's names do not meet).
// Windows: delta QueryThreadCycleTime(this thread) / delta rdtsc = the share of one core over the window, times the window's wall time (GetThreadTimes is tick-quantised: lane 306
// measured 5.94 % for a real 1.42 %; the same method as src\owner_hires_wait.h CpuMeter). Elsewhere: CLOCK_THREAD_CPUTIME_ID (already time).
// A test (tools\renderer\thread_cpu_test.cpp) runs this shipped code: a busy spin of N ms reads ~N ms, a sleep of N ms reads ~0.
#pragma once
#include <chrono>
#include <cstdint>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void* thread, unsigned long long* cycles);
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread(void);
#else
#include <time.h>
#endif

namespace rdr {

struct ThreadCpuMeter {
  int64_t a0 = 0, b0 = 0;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  static void Sample(int64_t& a, int64_t& b) {
#ifdef _WIN32
    unsigned long long cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    a = int64_t(cycles);
    b = int64_t(__builtin_ia32_rdtsc());
#else
    timespec ts;
    // render-clock: perf - per-thread CPU time for the profiling counters
    a = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec : 0;
    b = 0;
#endif
  }
  ThreadCpuMeter() { Sample(a0, b0); }
  // CPU ns this thread used since the last Take (or construction).
  uint64_t TakeNs() {
    int64_t a, b;
    Sample(a, b);
    const auto now = std::chrono::steady_clock::now();
    const double wall_ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count());
    t0 = now;
    const int64_t da = a - a0, db = b - b0;
    a0 = a;
    b0 = b;
#ifdef _WIN32
    return db > 0 && da > 0 ? uint64_t(wall_ns * double(da) / double(db)) : 0;
#else
    (void)wall_ns, (void)db;
    return da > 0 ? uint64_t(da) : 0;
#endif
  }
};

}  // namespace rdr
