// S102 lane R21-C step 3: the unit test of src/renderer_scene/crowd_workers.h (the pool that poses the crowd's classes). No game, no SDK.
//   crowd_workers_test.exe [--plant NAME]      exit 0 = all pass, 1 = a check failed (a plant must exit 1)
// What it proves: for 0, 1, 2 workers and 1..70 jobs over 300 runs, every job runs exactly once, its result lands in its own slot and equals the sequential run, and Run returns only when every job is done
// (a checksum of all slots is read the moment Run returns); the pool is reusable across runs of different sizes and across changes of the worker count; a late worker of an earlier run never touches the next run.
// Plants (each must fail): unjoined (Run returns before the pool is done: the slots are read too early), double_run (a job runs twice: the exactly-once count), drop_job (a job never runs).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_scene/crowd_workers.h"

using rdr_scene::CrowdWorkers;

static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
static uint64_t Work(size_t i) {  // a deterministic amount of arithmetic per job (a pose-like cost that differs per job)
  uint64_t h = 1469598103934665603ull ^ i;
  for (size_t k = 0; k < 2000 + (i % 7) * 800; ++k) h = (h ^ k) * 1099511628211ull;
  return h;
}

static const std::thread::id main_id = std::this_thread::get_id();

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  uint64_t runs_ok = 0, runs = 0, wrong = 0, count_bad = 0;
  {
    CrowdWorkers pool;
    for (int r = 0; r < 300; ++r) {
      const int workers = r % 3;
      const size_t jobs = size_t(1 + (r * 7) % 70);
      auto& slot = *new std::vector<uint64_t>(jobs, 0);  // (leaked on purpose: the unjoined twin leaves workers writing after Run returned)
      std::vector<uint64_t> want(jobs, 0);
      auto& hits = *new std::vector<std::atomic<int>>(jobs);
      for (auto& h : hits) h = 0;
      for (size_t i = 0; i < jobs; ++i) want[i] = Work(i);
      pool.Run(workers, jobs, [&slot, &hits, plant](size_t i) {
        if (plant == "drop_job" && i == 3) return;
        slot[i] = Work(i);
        hits[i].fetch_add(1);
        if (plant == "double_run" && i == 2) hits[i].fetch_add(1);
      }, plant == "unjoined" && workers > 0 && jobs >= 2);
      ++runs;
      bool same = true, once = true;
      for (size_t i = 0; i < jobs; ++i) same = same && slot[i] == want[i], once = once && hits[i].load() == 1;
      runs_ok += same && once;
      wrong += !same, count_bad += !once;
      if (plant == "unjoined") std::this_thread::sleep_for(std::chrono::milliseconds(6));  // (the twin's late workers finish before the next run's buffers are made)
    }
    Check(runs_ok == runs, "300 runs (0, 1, 2 workers; 1-70 jobs): every slot equals the sequential result the moment Run returns, every job ran exactly once (" + std::to_string(wrong) + " wrong slots, " + std::to_string(count_bad) + " runs with a job not run once)");
    {
      const std::vector<std::string> names = pool.Names();
      bool ok = !names.empty();
      for (const std::string& n : names) ok = ok && n.rfind("rdr_crowd_pose ", 0) == 0;
      Check(ok && names.size() == pool.Threads(), "every pool thread named itself \"rdr_crowd_pose N\" (" + std::to_string(names.size()) + " names for " + std::to_string(pool.Threads()) + " threads)");
    }
    Check(pool.Threads() <= 2, "the pool never holds more than 2 threads (" + std::to_string(pool.Threads()) + ")");
    Check(pool.Runs() > 0 && pool.Jobs() > pool.Runs(), "the pool ran the multi-job runs (" + std::to_string(pool.Runs()) + " runs, " + std::to_string(pool.Jobs()) + " jobs)");
  }
  {  // a worker preempted after it claimed a job: Run waits for it (the render thread's wall time is bounded by that worker, reported, never a wrong result)
    CrowdWorkers pool;
    std::vector<uint64_t> slot(8, 0);
    const auto t0 = std::chrono::steady_clock::now();
    pool.Run(2, 8, [&](size_t i) {
      if (i == 5 && std::this_thread::get_id() != main_id) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      slot[i] = Work(i);
    });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    bool same = true;
    for (size_t i = 0; i < 8; ++i) same = same && slot[i] == Work(i);
    std::printf("slow worker plant: Run took %.2f ms (a worker slept 5 ms inside a claimed job when it got job 5)\n", ms);
    Check(same, "a slow worker delays Run but every slot is right when it returns");
  }
  {  // a run with workers asked but a single job, and an empty run, stay on the calling thread
    CrowdWorkers pool;
    int n = 0;
    pool.Run(2, 1, [&](size_t) { ++n; });
    pool.Run(2, 0, [&](size_t) { ++n; });
    Check(n == 1 && pool.Threads() == 0, "one job or none never starts a thread");
  }
  std::printf("%s\n", g_fail ? "FAILED" : "all passed");
  return g_fail ? 1 : 0;
}
