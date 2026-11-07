// Session 95 (PACE): src\android_perf_hint_retry.h's plan against a faked APerformanceHint_createSession. Mirrors what
// PerfHint::UpdateThreads does at each 5 s scan: pick the threads, call create, report the failure.
#include <cstdio>
#include <vector>

#include "../../../src/android_perf_hint_retry.h"

using wwe2k14::perf_hint::CreateRetry;

static int failures = 0;
static void Report(bool ok, const char* what) {
  std::puts(ok ? "ok:   " : "FAIL: ");
  std::puts(what);
  if (!ok) ++failures;
}
#define CHECK(cond, what) Report((cond), what)

struct Fake {
  int fail_first;  // the first N creates fail
  int calls = 0;
  std::vector<std::vector<int>> seen;
  bool Create(const std::vector<int>& tids) {
    seen.push_back(tids);
    return ++calls > fail_first;
  }
};

// One scan: returns true once a session exists.
static bool Scan(CreateRetry& r, Fake& f, const std::vector<int>& live, bool& session) {
  if (session || r.Stopped()) return session;
  const auto tids = r.Pick(live, 200, live.front());
  if (tids.empty()) return false;
  if (f.Create(tids)) {
    r.Succeeded();
    session = true;
  } else {
    r.Failed(22);
  }
  return session;
}

int main() {
  const std::vector<int> live{100, 200, 300};
  {  // fails once: recovers on the second scan with the live threads
    CreateRetry r; Fake f{1}; bool s = false;
    CHECK(!Scan(r, f, live, s), "scan 1: create fails, hint not dead");
    CHECK(!r.Stopped(), "scan 1: still trying");
    CHECK(Scan(r, f, live, s), "scan 2: the create works (recovers on the second scan)");
    CHECK(f.seen[1] == live, "scan 2 uses the live threads");
  }
  {  // fails twice: the third try is the GPU thread alone
    CreateRetry r; Fake f{2}; bool s = false;
    Scan(r, f, live, s); Scan(r, f, live, s);
    CHECK(r.GpuAloneNext(), "after 2 failures: GPU thread alone next");
    CHECK(Scan(r, f, live, s), "scan 3: GPU alone works");
    CHECK(f.seen[2] == std::vector<int>{200}, "scan 3 asked with the GPU thread only");
  }
  {  // never works: stops after kMaxTries with the errno
    CreateRetry r; Fake f{99}; bool s = false;
    for (int i = 0; i < 10; ++i) Scan(r, f, live, s);
    CHECK(f.calls == CreateRetry::kMaxTries, "stops after kMaxTries creates");
    CHECK(r.Stopped() && r.LastErrno() == 22, "stopped, errno kept");
  }
  {  // no GPU thread seen: the busiest one
    CreateRetry r; r.Failed(1); r.Failed(1);
    CHECK(r.Pick(live, 0, 300) == std::vector<int>{300}, "no GPU thread by name: the busiest alone");
  }
  {  // a GPU-alone session that worked stays the plan at later scans (failures reset to 0 by the success)
    CreateRetry r; r.Failed(1); r.Failed(1); r.Succeeded();
    CHECK(!r.GpuAloneNext(), "after a success the failure count is 0");
    CHECK(r.Pick(live, 200, 300, true) == std::vector<int>{200}, "forced GPU-alone: still the GPU thread");
    CHECK(r.Pick(live, 200, 300, false) == live, "not forced: the live threads");
  }
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
