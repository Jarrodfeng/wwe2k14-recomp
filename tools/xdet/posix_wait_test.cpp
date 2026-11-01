// Session 77 (XDET_PLAN.md P5.2): unit test of the SDK's POSIX wait objects as det_serial uses them on Android,
// built against the SDK's own threading_posix.cpp (tools\xdet\run_posix_wait_test.ps1: NDK static build, run under
// QEMU in Docker). Checks:
//   1. rex::thread::PeekSignaled sees a signalled event / semaphore without consuming it (a Wait after it succeeds).
//   2. An auto-reset event peeked many times stays signalled until one Wait consumes it.
//   3. A mutant owned by thread A peeks signalled for A and not for B, from a third thread (the scheduler's view).
//   4. An alertable Wait with a 0 ms timeout on a signalled event succeeds (it returned kTimeout before Session 77).
//   5. An alertable 0 ms Wait on an unsignalled event times out at once; an alertable WaitMultiple(0 ms) on one
//      signalled of two succeeds.
// Prints one PASS / FAIL line per check and exits non-zero on any failure. -DXDET_OLD builds only checks 4-5 (no
// PeekSignaled) against an older threading_posix.cpp, to show the test catches the pre-Session-77 behaviour.
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace rex::thread;
using std::chrono::milliseconds;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

int main() {
#ifndef XDET_OLD
  const auto me = std::this_thread::get_id();
  {
    auto ev = Event::CreateManualResetEvent(false);
    Check(PeekSignaled(ev.get(), me) == 0, "1a manual event unsignalled peeks 0");
    ev->Set();
    Check(PeekSignaled(ev.get(), me) == 1, "1b manual event signalled peeks 1");
    Check(Wait(ev.get(), false, milliseconds(0)) == WaitResult::kSuccess, "1c wait after the peek succeeds");
  }
  {
    auto ev = Event::CreateAutoResetEvent(false);
    ev->Set();
    bool all = true;
    for (int i = 0; i < 100; ++i) all = all && PeekSignaled(ev.get(), me) == 1;
    Check(all, "2a auto-reset event: 100 peeks all 1 (nothing consumed)");
    Check(Wait(ev.get(), false, milliseconds(0)) == WaitResult::kSuccess, "2b one wait consumes it");
    Check(PeekSignaled(ev.get(), me) == 0, "2c after the wait it peeks 0");
  }
  {
    auto sem = Semaphore::Create(0, 4);
    Check(PeekSignaled(sem.get(), me) == 0, "1d semaphore count 0 peeks 0");
    int prev = -1;
    sem->Release(2, &prev);
    Check(PeekSignaled(sem.get(), me) == 1, "1e semaphore count 2 peeks 1");
    Check(Wait(sem.get(), false, milliseconds(0)) == WaitResult::kSuccess, "1f semaphore wait 1");
    Check(Wait(sem.get(), false, milliseconds(0)) == WaitResult::kSuccess, "1g semaphore wait 2");
    Check(PeekSignaled(sem.get(), me) == 0, "1h semaphore drained peeks 0");
  }
  {
    std::unique_ptr<Mutant> mu;
    std::thread::id a, b;
    std::atomic<int> stage{0};
    std::atomic<int> owner_asks_for_b{-9};
    std::atomic<bool> b_ready{false};
    // B stays alive until the end (a finished thread's id can be reused by the next one).
    std::thread tb([&] {
      b = std::this_thread::get_id();
      b_ready = true;
      while (stage.load() < 3) std::this_thread::yield();
    });
    while (!b_ready.load()) std::this_thread::yield();
    std::thread ta([&] {
      a = std::this_thread::get_id();
      mu = Mutant::Create(true);  // owned by A
      // The scheduler can evaluate a blocked thread's readiness on the owner's own thread: still B's view.
      owner_asks_for_b = PeekSignaled(mu.get(), b);
      stage = 1;
      while (stage.load() < 2) std::this_thread::yield();
      mu->Release();
      stage = 3;
    });
    while (stage.load() < 1) std::this_thread::yield();
    // This thread is neither A nor B: the scheduler's other position.
    Check(PeekSignaled(mu.get(), a) == 1, "3a owned mutant peeks 1 for its owner");
    Check(PeekSignaled(mu.get(), b) == 0, "3b owned mutant peeks 0 for another thread");
    Check(owner_asks_for_b.load() == 0, "3d owned mutant judged on the owner's thread for B peeks 0");
    stage = 2;
    while (stage.load() < 3) std::this_thread::yield();
    ta.join();
    Check(PeekSignaled(mu.get(), b) == 1, "3c released mutant peeks 1 for another thread");
    tb.join();
  }
  {
    // A timer handle (its native_handle() asserts on POSIX: the peek must not go there).
    auto timer = Timer::CreateManualResetTimer();
    Check(PeekSignaled(timer.get(), std::this_thread::get_id()) == 0, "3e unset timer peeks 0 (no assert)");
  }
#endif
  {
    auto ev = Event::CreateManualResetEvent(true);
    Check(Wait(ev.get(), true, milliseconds(0)) == WaitResult::kSuccess, "4 alertable 0 ms wait on a signalled event");
    auto off = Event::CreateManualResetEvent(false);
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = Wait(off.get(), true, milliseconds(0));
    const auto dt = std::chrono::steady_clock::now() - t0;
    Check(r == WaitResult::kTimeout && dt < milliseconds(50), "5a alertable 0 ms wait on an unsignalled event times out at once");
    auto e1 = Event::CreateManualResetEvent(false);
    auto e2 = Event::CreateManualResetEvent(true);
    WaitHandle* hs[2] = {e1.get(), e2.get()};
    const auto m = WaitMultiple(hs, 2, false, true, milliseconds(0));
    Check(m.first == WaitResult::kSuccess && m.second == 1, "5b alertable 0 ms WaitAny finds the signalled one");
  }
  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
  return g_fail ? 1 : 0;
}
