// S102 R25-A (wwe2k14_rdr_swap_tail2 bit 1): offline proof for the moved reader join. It runs the SHIPPED src\renderer_adapter\join_ticket.h Pool (the same class
// reader.h's ReaderWorkers derives from: Push / Pop / Finish / Mark / WaitBelow / WaitBelowUntil), threads and all.
//
// Model of the real thing (owner_timing.h + reader.h):
//   workers   Pop a job, read a slice of the "guest buffer" SLOWLY (a forced delay between reads: the race window), write what they read to the job's output
//   guest     (the swap thread): per frame writes the slice, queues a texture job (guest_reads = true), calls the fence hook, and then (like the game after a fence it has SEEN
//             answered) poisons the slice the moment the fence word reads this frame's number
//   hook      arm "today": WaitAll on the guest's thread, then push the item.  arm "moved": take a ticket (Mark), push the item with it.
//   timing    pops items in order; arm "moved": WaitBelowUntil(ticket, tick); then writes the fence word (the answer)
// Acceptance (no plant): every job's output equals the frame data it was queued for; outputs byte-equal between the arms; the lever took (the timing thread waited);
//   liveness: a texture job queued AFTER the ticket, and a GEOMETRY job (guest_reads = false) queued before it, both blocked, do not hold the item up;
//   vblank: one job held 100 ms on the timing thread's wait: the fake 60 Hz clock's vblanks are dispatched DURING the wait (>= 5) with no catch-up burst > 1;
//   fall back: the SDK processor writes fence k + 2 directly after FellBack settled the pool (WaitAll): the guest never sees a fence while a job reads;
//   narrowed guarantee (documented, not a failure): a guest that poisons the slice right after the HOOK returns (not after the fence) is safe today and NOT safe moved.
// Planted twins (`swap_tail2_test plant N`, each must exit 1 with a FAIL line):
//   1 the timing thread answers without waiting     2 the ticket is taken before this frame's job is queued     3 the ticket is one short
//   4 FellBack without the WaitAll                  5 the timing thread waits blind (WaitBelow: no vblank during the wait)
//   6 a geometry job takes a ticket (guest_reads = true for a job that reads no guest memory)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/join_ticket.h"

using Clock = std::chrono::steady_clock;
static int g_plant = 0;
static int g_fail = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL %s: ", #cond);                    \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
      ++g_fail;                                           \
    }                                                     \
  } while (0)

constexpr uint32_t kSlice = 64;

static void Spin(int us) {
  const auto end = Clock::now() + std::chrono::microseconds(us);
  while (Clock::now() < end) {
  }
}

struct Job {
  uint64_t seq = 0;
  const volatile uint8_t* src = nullptr;
  uint8_t out[kSlice] = {};
  std::atomic<bool> hold{false};  // the worker blocks until this is cleared
};
using PoolT = wwe2k14::join_ticket::Pool<Job>;

struct Workers : PoolT {
  std::vector<std::thread> th;
  void Start(int n) {
    for (int i = 0; i < n; ++i) th.emplace_back([this, i] { Loop(i); });
  }
  void Loop(int slot) {
    for (;;) {
      Job* j = Pop(slot);
      if (!j) return;
      if (j->src)
        for (uint32_t k = 0; k < kSlice; ++k) {  // the slow read: the race window (each chunk its own chance for the guest to overwrite)
          j->out[k] = j->src[k];
          if ((k & 15) == 0) Spin(40);  // (a spin, not sleep_for: Windows sleeps are 1-15 ms granular, which made a 300-frame arm take 20 s)
        }
      while (j->hold.load()) std::this_thread::yield();
      Finish(slot);
    }
  }
  uint64_t MarkP() {
    const uint64_t m = Mark();
    return (g_plant == 3 && m) ? m - 1 : m;  // plant 3: the ticket is one short
  }
  void Stop() {
    {
      std::lock_guard lock(mu);
      stop = true;
    }
    work_cv.notify_all();
    for (auto& t : th) t.join();
  }
};

struct Item {
  uint32_t frame = 0;
  uint64_t join = 0;
};

struct Result {
  std::vector<uint8_t> out;
  uint64_t tickets = 0, timing_waits = 0;
  bool bad = false;
};

static uint8_t Expect(int f, uint32_t k) { return uint8_t(f * 31 + k * 7 + 1); }  // never 0xEE (the poison)

// One arm. moved = the ticket way. poison_after_hook: the guest poisons as soon as the hook returned (the narrowed-guarantee twin).
static Result RunArm(bool moved, int frames, int nworkers, bool plant_active, bool poison_after_hook = false) {
  Workers w;
  w.Start(nworkers);
  std::vector<uint8_t> guest(kSlice * 4);
  std::mutex qmu;
  std::condition_variable qcv;
  std::deque<Item> q;
  std::atomic<uint32_t> fence_word{0};
  std::atomic<bool> timing_stop{false};
  std::atomic<uint64_t> timing_waits{0};
  Result res;
  std::atomic<bool> arm_done{false};
  std::atomic<int> at_frame{0};
  std::thread watchdog([&] {  // a hang is a FAIL with the pool's state, not a stuck gate
    for (int i = 0; i < 600 && !arm_done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (arm_done.load()) return;
    std::lock_guard lock(w.mu);
    std::printf("FAIL arm hung (moved %d, plant %d): guest frame %d, fence word %u, running %zu, queued %zu, running_seq %llu %llu %llu, next %llu\n", int(moved), g_plant, at_frame.load(),
                fence_word.load(), w.running, w.queue.size(), (unsigned long long)w.running_seq[0], (unsigned long long)w.running_seq[1], (unsigned long long)w.running_seq[2],
                (unsigned long long)w.next);
    std::fflush(stdout);
    std::_Exit(1);
  });

  std::thread timing([&] {
    for (;;) {
      Item it;
      {
        std::unique_lock lock(qmu);
        qcv.wait(lock, [&] { return !q.empty() || timing_stop.load(); });
        if (q.empty()) return;
        it = q.front();
        q.pop_front();
      }
      if (it.join && !(plant_active && g_plant == 1)) {  // plant 1: the wait is skipped
        const uint32_t ticks = w.WaitBelowUntil(it.join, [] { return Clock::now() + std::chrono::milliseconds(2); });
        if (ticks) timing_waits.fetch_add(1);
      }
      fence_word.store(it.frame, std::memory_order_release);  // the answer: the guest may now see it
    }
  });

  std::vector<Job> jobs(frames);
  for (int f = 1; f <= frames; ++f) {
    at_frame.store(f);
    uint8_t* slice = guest.data() + (f % 4) * kSlice;
    for (uint32_t k = 0; k < kSlice; ++k) slice[k] = Expect(f, k);
    uint64_t ticket_early = 0;
    if (moved && plant_active && g_plant == 2) ticket_early = w.MarkP();  // plant 2: the ticket before this frame's job is queued
    jobs[f - 1].src = slice;
    w.Push(&jobs[f - 1], true);
    Item it;
    it.frame = uint32_t(f);
    if (moved) {
      it.join = (plant_active && g_plant == 2) ? ticket_early : w.MarkP();
      ++res.tickets;
    } else {
      w.WaitAll();
    }
    {
      std::lock_guard lock(qmu);
      q.push_back(it);
    }
    qcv.notify_one();
    if (!poison_after_hook) while (fence_word.load(std::memory_order_acquire) < uint32_t(f)) std::this_thread::yield();  // the game waits for the fence it saw answered
    std::memset(slice, 0xEE, kSlice);                                                                                   // ... and reuses the memory
    if (poison_after_hook) while (fence_word.load(std::memory_order_acquire) < uint32_t(f)) std::this_thread::yield();
  }
  w.WaitAll();
  {
    std::lock_guard lock(qmu);
    timing_stop.store(true);
  }
  qcv.notify_all();
  timing.join();
  w.Stop();
  arm_done.store(true);
  watchdog.join();
  res.timing_waits = timing_waits.load();
  for (int f = 1; f <= frames; ++f)
    for (uint32_t k = 0; k < kSlice; ++k) {
      res.out.push_back(jobs[f - 1].out[k]);
      if (jobs[f - 1].out[k] != Expect(f, k)) res.bad = true;
    }
  return res;
}

// Liveness: neither a texture job queued AFTER the ticket nor a geometry job (reads no guest memory) queued BEFORE it may hold the item up.
static void Liveness() {
  Workers w;
  w.Start(3);
  uint8_t a[kSlice] = {1};
  Job geo, ja, later;
  geo.hold.store(true);
  geo.src = nullptr;  // a geometry build: no guest read
  w.Push(&geo, g_plant == 6);  // plant 6: it takes a ticket although it reads no guest memory
  ja.src = a;
  w.Push(&ja, true);
  const uint64_t ticket = w.Mark();  // covers ja only (and geo under plant 6)
  later.hold.store(true);
  later.src = nullptr;
  w.Push(&later, true);  // a guest-reading job queued AFTER the ticket
  std::atomic<bool> ok{false};
  std::thread waiter([&] {
    w.WaitBelow(ticket);
    ok = true;
  });
  const auto t0 = Clock::now();
  while (!ok && Clock::now() - t0 < std::chrono::seconds(3)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(ok.load(), "the ticket waited for a blocked geometry job or for a job queued after it (blocked for 3 s)");
  geo.hold.store(false);
  later.hold.store(false);
  waiter.join();
  w.WaitAll();
  w.Stop();
  if (ok) std::printf("PASS liveness: a ticket does not wait for a later job, nor for a geometry job (both blocked, item answered)\n");
}

// The vblank: a 60 Hz fake clock; the timing thread's tick dispatches the vblanks owed (counting each tick's burst) and returns the next deadline.
struct FakeVblank {
  Clock::time_point start = Clock::now(), next = Clock::now() + std::chrono::microseconds(16667);
  std::atomic<uint32_t> dispatched{0}, max_burst{0};
  Clock::time_point Tick() {
    uint32_t n = 0;
    while (Clock::now() >= next) {
      next += std::chrono::microseconds(16667);
      ++n;
    }
    dispatched += n;
    if (n > max_burst) max_burst = n;
    return next;
  }
};
static void VblankDuringWait() {
  Workers w;
  w.Start(1);
  uint8_t a[kSlice] = {1};
  Job j;
  j.src = a;
  j.hold.store(true);
  w.Push(&j, true);
  const uint64_t ticket = w.Mark();
  FakeVblank vb;
  std::thread rel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    j.hold.store(false);
  });
  if (g_plant == 5) w.WaitBelow(ticket);  // plant 5: blind
  else w.WaitBelowUntil(ticket, [&] { return vb.Tick(); });
  vb.Tick();  // (one last tick: what fell due at the end)
  const uint32_t d = vb.dispatched.load();
  rel.join();
  w.WaitAll();
  w.Stop();
  CHECK(d >= 5, "only %u vblanks were dispatched during a 100 ms wait (expected >= 5)", d);
  CHECK(vb.max_burst.load() <= 1, "a catch-up burst of %u vblanks", vb.max_burst.load());
  if (d >= 5 && vb.max_burst.load() <= 1)
    std::printf("PASS vblank: a job held 100 ms: %u vblanks dispatched during the wait, largest burst %u\n", d, vb.max_burst.load());
}

// FellBack: fence k moved (ticket on the queue), then the SDK processor writes fence k + 2 itself. Settled first (WaitAll in FellBack): the guest never sees a fence while its job reads.
static void FellBackArm() {
  Workers w;
  w.Start(2);
  std::vector<uint8_t> guest(kSlice);
  std::atomic<uint32_t> fence_word{0};
  Item it;
  for (uint32_t k = 0; k < kSlice; ++k) guest[k] = Expect(1, k);
  Job j;
  j.src = guest.data();
  w.Push(&j, true);
  it.frame = 1;
  it.join = w.Mark();  // item k is on the queue with its ticket; the timing thread is slow to get to it (below it only starts after the processor wrote)
  if (g_plant != 4) w.WaitAll();  // FellBack: settle every ticket before the processor runs (plant 4: not)
  fence_word.store(3, std::memory_order_release);  // the SDK processor writes fence k + 2 directly
  while (fence_word.load(std::memory_order_acquire) < 1) std::this_thread::yield();  // the guest sees a fence >= k
  std::memset(guest.data(), 0xEE, kSlice);                                           // and reuses the memory
  w.WaitAll();
  w.Stop();
  bool bad = false;
  for (uint32_t k = 0; k < kSlice; ++k)
    if (j.out[k] != Expect(1, k)) bad = true;
  CHECK(!bad, "the job read memory the guest reused after the processor's direct fence write");
  if (!bad) std::printf("PASS fell back: the processor's direct fence write came after the pool was settled; the job read its own frame\n");
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 3 && std::strcmp(argv[1], "plant") == 0) g_plant = std::atoi(argv[2]);
  const bool planted = g_plant != 0;
  const int frames = 300;

  if (!planted) {
    Liveness();
    VblankDuringWait();
    FellBackArm();
    for (int nw : {1, 3}) {
      Result a = RunArm(false, frames, nw, false);
      Result b = RunArm(true, frames, nw, false);
      CHECK(!a.bad, "today's arm (%d workers) read poisoned memory", nw);
      CHECK(!b.bad, "the moved arm (%d workers) read poisoned memory", nw);
      CHECK(a.out == b.out, "outputs differ between the arms (%d workers)", nw);
      CHECK(b.tickets == uint64_t(frames), "tickets %llu", (unsigned long long)b.tickets);
      CHECK(b.timing_waits > 0, "the timing thread never had to wait (the lever was not exercised, %d workers)", nw);
      if (!g_fail)
        std::printf("PASS %d workers: %d frames, outputs byte-equal off vs on (%zu bytes), no poisoned read, the timing thread waited %llu times (the lever took)\n", nw, frames,
                    b.out.size(), (unsigned long long)b.timing_waits);
    }
    // the narrowed guarantee, documented: poisoning right after the HOOK returns is safe today (WaitAll) and not moved
    bool today_ok = true, moved_bad = false;
    for (int t = 0; t < 6; ++t) {
      if (RunArm(false, 120, 3, false, true).bad) today_ok = false;
      if (RunArm(true, 120, 3, false, true).bad) moved_bad = true;
    }
    CHECK(today_ok, "today's arm read poison although the guest waited for nothing: the model is wrong");
    CHECK(moved_bad, "the moved arm survived a guest that poisons right after the hook: the narrowed-guarantee twin does not show it");
    if (today_ok && moved_bad) std::printf("PASS narrowed guarantee documented: a guest that reuses memory right after the hook returns (no fence seen) is safe today and NOT safe moved\n");
  } else if (g_plant == 6) {
    Liveness();
  } else if (g_plant == 5) {
    VblankDuringWait();
  } else if (g_plant == 4) {
    // the processor writes before the pool is settled: caught when the job is still reading (a few tries; the slow read makes the window wide)
    for (int t = 0; t < 8 && !g_fail; ++t) FellBackArm();
  } else {
    bool caught = false;
    for (int t = 0; t < 8 && !caught; ++t)
      if (RunArm(true, frames, 3, true).bad) caught = true;
    CHECK(!caught, "plant %d: a job read memory the guest had already reused (the planted fault is caught)", g_plant);
  }
  if (!g_fail) std::printf("all checks passed\n");
  return g_fail ? 1 : 0;
}
