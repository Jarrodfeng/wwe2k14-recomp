// S102 R25-A (docs\renderer\SWAP_TAIL.md section 9): the reader's worker pool and which of its jobs a fence / interrupt / flip must wait for, as a ticket. No game or SDK
// includes: reader.h's ReaderWorkers (derived from Pool) and tools\swap_tail2\swap_tail2_test.cpp run THIS code.
//
// Today (owner_timing.h JoinReader) the game thread that calls the fence / interrupt / flip hook waits until EVERY reader job has finished (WaitAll: running == 0).
// The wait protects one thing: the guest must not see the fence (and then reuse the memory a texture job is still reading) before the jobs that READ GUEST MEMORY and were
// queued BEFORE the fence are done. With wwe2k14_rdr_swap_tail2 bit 1 the hook takes a ticket (Mark: "every guest-reading job queued so far") and carries it on the queue
// item; the "Owner timing" thread waits for the ticket right before it answers the item (the only place the guest can observe the fence), dispatching the vblanks that fall
// due while it waits (WaitBelowUntil). Jobs queued after the Mark, and jobs that read no guest memory (geometry builds read copies made at the draw), are not waited for.
//
// No allocation on the job path: a guest-reading job gets a sequence number (jobs that read none get 0); the smallest pending one is the first in the FIFO queue with a
// sequence plus the (at most kSlots) sequences the workers are running. Everything under the pool's mutex.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

namespace wwe2k14::join_ticket {

inline constexpr int kSlots = 8;  // the most worker threads (reader.h clamps wwe2k14_rdr_reader_workers to 0..8)

// J: a job type with `uint64_t seq`.
template <class J>
struct Pool {
  std::mutex mu;
  std::condition_variable work_cv, done_cv;
  std::deque<J*> queue;
  size_t running = 0;  // queued or being run
  bool stop = false;
  uint64_t next = 1;                       // the sequence the next guest-reading job gets (0 is "no ticket" / "reads no guest memory")
  uint64_t running_seq[kSlots] = {};       // per worker slot: the sequence it is running (0 = none or a job that reads no guest memory)

  // The caller keeps the job alive until WaitAll.
  void Push(J* job, bool guest_reads) {
    {
      std::lock_guard lock(mu);
      job->seq = guest_reads ? next++ : 0;
      queue.push_back(job);
      ++running;
    }
    work_cv.notify_one();
  }
  // A worker: the next job, or nullptr when stopped and drained.
  J* Pop(int slot) {
    std::unique_lock lock(mu);
    work_cv.wait(lock, [&] { return stop || !queue.empty(); });
    if (stop && queue.empty()) return nullptr;
    J* j = queue.front();
    queue.pop_front();
    running_seq[slot] = j->seq;
    return j;
  }
  // A worker, after the job ran (before the lock goes the game thread's WaitAll may free the job once running reads 0).
  void Finish(int slot) {
    {
      std::lock_guard lock(mu);
      --running;
      running_seq[slot] = 0;
    }
    done_cv.notify_all();
  }
  void WaitAll() {
    std::unique_lock lock(mu);
    done_cv.wait(lock, [&] { return running == 0; });
  }

  // Under mu: the smallest pending guest-reading sequence, 0 when none.
  uint64_t MinPendingLocked() const {
    uint64_t m = 0;
    for (int i = 0; i < kSlots; ++i)
      if (running_seq[i] && (!m || running_seq[i] < m)) m = running_seq[i];
    for (const J* j : queue)  // FIFO: the first guest-reading job is the smallest queued one
      if (j->seq) {
        if (!m || j->seq < m) m = j->seq;
        break;
      }
    return m;
  }
  // The ticket for "every guest-reading job queued so far": 0 when none is pending (nothing to wait for).
  uint64_t Mark() {
    std::lock_guard lock(mu);
    return MinPendingLocked() ? next : 0;
  }
  bool ReadyLocked(uint64_t mark) const {
    if (!mark) return true;
    const uint64_t m = MinPendingLocked();
    return !m || m >= mark;
  }
  void WaitBelow(uint64_t mark) {
    std::unique_lock lock(mu);
    done_cv.wait(lock, [&] { return ReadyLocked(mark); });
  }
  // The Owner timing thread's wait: `tick()` is called with no lock held, first and after every timeout; it dispatches whatever is due (the vblanks) and returns the
  // time point to wake at next. The wait ends as soon as the ticket is ready. Returns the number of ticks that ran (0 = the jobs were done at the first look).
  template <class Tick>
  uint32_t WaitBelowUntil(uint64_t mark, Tick&& tick) {
    uint32_t ticks = 0;
    for (;;) {
      {
        std::lock_guard lock(mu);
        if (ReadyLocked(mark)) return ticks;
      }
      const std::chrono::steady_clock::time_point until = tick();
      ++ticks;
      std::unique_lock lock(mu);
      if (done_cv.wait_until(lock, until, [&] { return ReadyLocked(mark); })) return ticks;
    }
  }
};

}  // namespace wwe2k14::join_ticket
