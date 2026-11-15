// S102 lane R21-C step 3 (switch wwe2k14_rdr_crowd_workers, 0-2, per frame, default 0): a small pool that runs the crowd members' per-class pose (CrowdOurs::PoseVertices: a pure function of one class's
// meshes and palette, writing only that class's own posed vectors) on 0-2 worker threads plus the calling thread, and returns only when every job is done ("joined before PushGeometry").
// Run() is synchronous: the render thread never carries a pending job into the rest of the frame. One owner (CrowdOurs), one caller (the render thread). Each run is its own heap Job (a copy of the function,
// its own counters), so a worker that wakes late for an earlier run finds that run finished and touches nothing of the next. No SDK, no game: tools\crowd\crowd_workers_test.cpp.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <cwchar>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "../thread_name.h"  // S102 H1: every thread of ours is named (R21-C review F1)

#if defined(_WIN32)
// kernel32's SetThreadPriority under another name (no windows.h: its macros break other headers); the pseudo-handle -2 is the calling thread.
extern "C" int __stdcall wwe2k14_r21c_SetThreadPriority(void* thread, int priority) __asm__("SetThreadPriority");
#endif

namespace rdr_scene {

class CrowdWorkers {
 public:
  CrowdWorkers() = default;
  CrowdWorkers(const CrowdWorkers&) = delete;
  CrowdWorkers& operator=(const CrowdWorkers&) = delete;
  ~CrowdWorkers() { Stop(); }

  // Runs fn(0 .. jobs-1) on the calling thread and up to `workers` (max 2) pool threads, and returns when every job is done. workers <= 0 or fewer than 2 jobs: all on the calling thread. Jobs touch only
  // their own index's data. `skip_join` is the planted twin (workers_unjoined): it returns before the pool is done, and the pool is made late, so the loss is certain.
  void Run(int workers, size_t jobs, const std::function<void(size_t)>& fn, bool skip_join = false) {
    if (workers <= 0 || jobs < 2) {
      for (size_t i = 0; i < jobs; ++i) fn(i);
      return;
    }
    if (workers > 2) workers = 2;
    Ensure(size_t(workers));
    auto job = std::make_shared<Job>();
    job->fn = fn, job->jobs = jobs, job->slow = skip_join;
    {
      std::lock_guard<std::mutex> lock(m_);
      cur_ = job, active_ = size_t(workers);
      ++generation_;
    }
    cv_.notify_all();
    ++runs_, ran_jobs_ += jobs;
    if (skip_join) return;
    Drain(*job);  // the calling thread takes jobs too
    std::unique_lock<std::mutex> lock(m_);
    cv_done_.wait(lock, [&] { return job->done.load() >= job->jobs; });
  }
  // Blocks until the latest run is done (the planted workers_unjoined twin returns early from Run; its caller joins here before the data the jobs touch goes out of scope).
  void WaitIdle() {
    std::shared_ptr<Job> job;
    {
      std::lock_guard<std::mutex> lock(m_);
      job = cur_;
    }
    if (!job) return;
    std::unique_lock<std::mutex> lock(m_);
    cv_done_.wait(lock, [&] { return job->done.load() >= job->jobs; });
  }
  // The names the pool's threads gave themselves ("rdr_crowd_pose N"), for the unit test and the thread log.
  std::vector<std::string> Names() {
    std::lock_guard<std::mutex> lock(m_);
    return names_;
  }
  uint64_t Runs() const { return runs_; }
  uint64_t Jobs() const { return ran_jobs_; }
  size_t Threads() const { return threads_.size(); }

 private:
  struct Job {
    std::function<void(size_t)> fn;
    size_t jobs = 0;
    bool slow = false;
    std::atomic<size_t> next{0}, done{0};
  };
  void Ensure(size_t n) {
    while (threads_.size() < n) {
      const size_t id = threads_.size();
      threads_.emplace_back([this, id] { Loop(id); });
    }
  }
  void Drain(Job& job) {
    for (;;) {
      const size_t i = job.next.fetch_add(1);
      if (i >= job.jobs) break;
      job.fn(i);
      if (job.done.fetch_add(1) + 1 >= job.jobs) {
        std::lock_guard<std::mutex> lock(m_);
        cv_done_.notify_all();
      }
    }
  }
  void Loop(size_t id) {
    wchar_t name[32];
    std::swprintf(name, 32, L"rdr_crowd_pose %zu", id);
    wwe2k14::NameThread(name);   // (R21-C review F1: the first statement, so thread_busy / threads.csv name it)
#if defined(_WIN32)
    wwe2k14_r21c_SetThreadPriority(reinterpret_cast<void*>(static_cast<intptr_t>(-2)), -1);   // THREAD_PRIORITY_BELOW_NORMAL: a worker never preempts the game's own threads on a woken wait boost
#endif
    {
      std::string narrow;
      for (const wchar_t* p = name; *p; ++p) narrow.push_back(char(*p));
      std::lock_guard<std::mutex> lock(m_);
      names_.push_back(narrow);
    }
    uint64_t seen = 0;
    for (;;) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
        if (id >= active_) continue;
        job = cur_;
      }
      if (job->slow) std::this_thread::sleep_for(std::chrono::milliseconds(4));  // (the twin: the pool is late, so a caller that does not join sees unfinished jobs)
      Drain(*job);
    }
  }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : threads_) t.join();
    threads_.clear();
  }

  std::mutex m_;
  std::condition_variable cv_, cv_done_;
  std::vector<std::thread> threads_;
  std::shared_ptr<Job> cur_;
  std::vector<std::string> names_;
  size_t active_ = 0;
  uint64_t generation_ = 0, runs_ = 0, ran_jobs_ = 0;
  bool stop_ = false;
};

}  // namespace rdr_scene
