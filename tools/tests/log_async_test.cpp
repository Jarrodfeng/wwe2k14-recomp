// Session 101 lane 302 offline test: the async log sink (rex::AsyncSink, SDK include/rex/logging/async_sink.h).
// Answers: (1) before: a blocked disk write blocks the logging thread; after: < 1 ms per line.  (2) warn/error lines all reach the
// file even with a full queue while info lines are dropped and counted.  (3) the crash path (Drain) waits for everything queued,
// and gives up after its timeout instead of hanging.  (4) per-line cost, sync vs async, against a real file.
// Build + run: tools\tests\log_async_test.ps1 (no game, no SDK build). Exit code 0 = all checks passed.
#define SPDLOG_HEADER_ONLY
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <rex/logging/async_sink.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s: ", #c); std::printf(__VA_ARGS__); std::printf("\n"); } else { std::printf("ok   %s\n", #c); } } while (0)

// A sink whose first `slow_writes` writes sleep `sleep_ms` (the stalled disk), forwarding to `inner` afterwards.
class SlowSink : public spdlog::sinks::base_sink<std::mutex> {
 public:
  SlowSink(std::shared_ptr<spdlog::sinks::sink> inner, int slow_writes, int sleep_ms)
      : inner_(std::move(inner)), slow_(slow_writes), sleep_ms_(sleep_ms) {}
  std::atomic<int> writes{0};
 protected:
  void sink_it_(const spdlog::details::log_msg& m) override {
    if (slow_ > 0) { --slow_; std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms_)); }
    ++writes;
    if (inner_) inner_->log(m);
  }
  void flush_() override { if (inner_) inner_->flush(); }
 private:
  std::shared_ptr<spdlog::sinks::sink> inner_;
  int slow_, sleep_ms_;
};

static int CountLines(const std::string& path, const char* needle) {
  std::ifstream f(path);
  std::string l; int n = 0;
  while (std::getline(f, l)) if (l.find(needle) != std::string::npos) ++n;
  return n;
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  // ---- 1. BEFORE: synchronous sink, writes block 2 s each (the run-7202 shape) ----
  {
    auto slow = std::make_shared<SlowSink>(nullptr, 1000, 2000);
    spdlog::logger lg("before", slow);
    lg.flush_on(spdlog::level::info);
    const auto t0 = Clock::now();
    lg.info("line 1");
    lg.info("line 2");
    const double per = Ms(t0, Clock::now()) / 2;
    std::printf("before: %.1f ms per info line with a 2000 ms write\n", per);
    CHECK(per > 1500.0, "per=%.1f (the old path must block)", per);
  }
  // ---- 2. AFTER: the same stalled sink behind AsyncSink ----
  {
    auto slow = std::make_shared<SlowSink>(nullptr, 1, 2000);  // first write blocks 2 s
    auto as = std::make_shared<rex::AsyncSink>(slow);
    spdlog::logger lg("after", as);
    lg.flush_on(spdlog::level::info);
    lg.info("first");  // writer picks this up and blocks 2 s
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    double worst = 0, sum = 0;
    const int N = 2000;
    for (int i = 0; i < N; ++i) {
      const auto t0 = Clock::now();
      lg.info("blocked-disk line {}", i);
      const double d = Ms(t0, Clock::now());
      sum += d; if (d > worst) worst = d;
    }
    std::printf("after : mean %.4f ms, worst %.4f ms per info line while the write is blocked\n", sum / N, worst);
    CHECK(worst < 1.0, "worst=%.4f ms", worst);
    CHECK(as->Drain(10000), "drain after the stall ends");
    CHECK(slow->writes == N + 1, "writes=%d", slow->writes.load());
  }
  // ---- 3. warn/error never dropped, info dropped + counted on a full queue, crash-path drain ----
  {
    const std::string path = dir + "/log_async_test.log";
    std::remove(path.c_str());
    auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
    auto slow = std::make_shared<SlowSink>(file, 1, 1500);
    rex::AsyncSink::Options opt; opt.max_queue = 100;
    auto as = std::make_shared<rex::AsyncSink>(slow, opt);
    spdlog::logger lg("q", as);
    lg.flush_on(spdlog::level::info);
    lg.info("seed");  // blocks the writer 1.5 s
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int warns = 0, errs = 0;
    for (int i = 0; i < 1000; ++i) {
      lg.info("flood-info {}", i);
      if (i % 20 == 0) { lg.warn("flood-warn {}", i); ++warns; }
      if (i % 50 == 0) { lg.error("flood-error {}", i); ++errs; }
    }
    // crash path: Drain gives up after its timeout instead of hanging on the stalled disk
    const auto t0 = Clock::now();
    const bool early = as->Drain(100);
    const double waited = Ms(t0, Clock::now());
    CHECK(!early && waited < 400.0, "early=%d waited=%.0f ms (must return false near 100 ms)", early, waited);
    CHECK(as->Drain(10000), "full drain once the disk answers");
    const auto st = as->TakeStats();
    std::printf("queue : queued %llu dropped %llu worst_write %.1f ms worst_caller %.3f ms\n", (unsigned long long)st.lines_queued,
                (unsigned long long)st.lines_dropped, st.worst_write_us / 1000.0, st.worst_caller_us / 1000.0);
    CHECK(st.lines_dropped > 0, "dropped=%llu", (unsigned long long)st.lines_dropped);
    CHECK(CountLines(path, "flood-warn") == warns, "warn lines in file=%d expected %d", CountLines(path, "flood-warn"), warns);
    CHECK(CountLines(path, "flood-error") == errs, "error lines in file=%d expected %d", CountLines(path, "flood-error"), errs);
    CHECK(CountLines(path, "async sink dropped") == 1, "the dropped-lines warn line is written once");
    CHECK(CountLines(path, "flood-info") + (int)st.lines_dropped == 1000, "info in file %d + dropped %llu == 1000", CountLines(path, "flood-info"),
          (unsigned long long)st.lines_dropped);
  }
  // ---- 3b. a CRITICAL line (what precedes abort() / _Exit()) is on disk when log() returns; a warn storm is capped, error never ----
  {
    const std::string path = dir + "/log_async_test_crit.log";
    std::remove(path.c_str());
    auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
    auto slow = std::make_shared<SlowSink>(file, 1, 300);
    rex::AsyncSinkOptions opt; opt.max_queue = 10; opt.warn_cap = 20;
    auto as = std::make_shared<rex::AsyncSink>(slow, opt);
    spdlog::logger lg("c", as);
    lg.flush_on(spdlog::level::info);
    lg.info("seed");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (int i = 0; i < 100; ++i) lg.warn("storm-warn {}", i);
    for (int i = 0; i < 5; ++i) lg.error("storm-error {}", i);
    const auto t0 = Clock::now();
    lg.critical("the fatal line");  // blocks until written (<= 1 s)
    const double waited = Ms(t0, Clock::now());
    std::printf("crit  : critical returned after %.0f ms\n", waited);
    CHECK(CountLines(path, "the fatal line") == 1, "the CRITICAL line is in the file when log() returns");
    CHECK(CountLines(path, "storm-error") == 5, "errors never dropped (%d)", CountLines(path, "storm-error"));
    CHECK(CountLines(path, "storm-warn") <= 21 && CountLines(path, "storm-warn") >= 1, "warns capped (%d of 100)", CountLines(path, "storm-warn"));
    // destructor is non-blocking even with a stalled writer
    auto slow2 = std::make_shared<SlowSink>(nullptr, 1, 1500);
    auto t1 = Clock::now();
    {
      rex::AsyncSink gone(slow2);
      spdlog::logger l2("d", std::shared_ptr<spdlog::sinks::sink>(&gone, [](auto*) {}));
      l2.info("blocks the writer");
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      t1 = Clock::now();
    }
    const double dtor = Ms(t1, Clock::now());
    CHECK(dtor < 50.0, "~AsyncSink took %.1f ms with a stalled writer", dtor);
  }
  // ---- 4. per-line cost against a real file, flush on every info line ----
  {
    const int N = 20000;
    auto run = [&](bool async) {
      const std::string path = dir + (async ? "/log_bench_async.log" : "/log_bench_sync.log");
      std::remove(path.c_str());
      auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
      std::shared_ptr<spdlog::sinks::sink> s = file;
      std::shared_ptr<rex::AsyncSink> as;
      if (async) { as = std::make_shared<rex::AsyncSink>(file); s = as; }
      spdlog::logger lg("b", s);
      lg.flush_on(spdlog::level::info);
      double worst = 0;
      const auto t0 = Clock::now();
      for (int i = 0; i < N; ++i) {
        const auto a = Clock::now();
        lg.info("[wwe2k14][perf] bench line {} some typical payload text here {}", i, 3.14159 * i);
        const double d = Ms(a, Clock::now()); if (d > worst) worst = d;
      }
      const double total = Ms(t0, Clock::now());
      if (as) as->Drain(10000);
      std::printf("bench : %-5s %.2f us/line on the caller, worst %.3f ms (%d lines)\n", async ? "async" : "sync", total * 1000.0 / N, worst, N);
      return total / N;
    };
    const double sync_ms = run(false), async_ms = run(true);
    CHECK(async_ms < sync_ms, "async %.4f ms/line must beat sync %.4f", async_ms, sync_ms);
  }
  std::printf(g_fail ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
