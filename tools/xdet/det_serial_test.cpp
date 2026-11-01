// Session 77 (XDET_PLAN.md P5.2, SDK review): unit test of the SDK's det_serial scheduler (det.cpp + det_serial.cpp)
// on arm64, built and run like tools\xdet\posix_wait_test.cpp (tools\xdet\run_posix_wait_test.ps1: NDK static, QEMU
// in Docker). Plain host threads stand in for guest threads; a vblank thread runs SerialWaitIdle / NoteVblank /
// SerialAfterVblank as the SDK's vblank thread does. Checks:
//   1. Round-robin: thread A takes the first turn and creates B and C in it; all three stay ready and run in creation
//      order, A B C A B C ... (10 rounds).
//   2. Deadline: a thread blocked until det vblank V+3 (its ready() false) runs at exactly V+3.
//   3. Killed slot (SerialThreadKilled, Session 77): E blocks forever; F waits for E's slot to exit
//      (SerialSlotExitedLocked, as XObject::Wait on a thread object). F does not run before the kill, runs after it;
//      SerialSlotExited(E) turns true.
//   4. Drain check removal (RemoveDrainCheck, Session 77): a drain check that says "not drained" holds the next
//      decision (G does not run); once removed, G runs and the check is never called again.
// Prints one PASS / FAIL line per check and exits non-zero on any failure.
#include <rex/cvar.h>
#include <rex/det.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

REXCVAR_DECLARE(bool, det_frames);
REXCVAR_DECLARE(bool, det_serial);

namespace det = rex::det;
using namespace std::chrono_literals;

static int g_failures = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  std::fflush(stdout);
  if (!ok) ++g_failures;
}

static std::mutex g_order_mu;
static std::string g_order;
static void Note(char c) {
  std::lock_guard<std::mutex> lock(g_order_mu);
  g_order += c;
}
static bool Noted(char c) {
  std::lock_guard<std::mutex> lock(g_order_mu);
  return g_order.find(c) != std::string::npos;
}
// Polls (host time) until done() or the timeout.
template <typename F>
static bool WaitFor(F done, std::chrono::milliseconds timeout) {
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() > until) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

static void Worker(char id, int slot, int rounds) {
  det::SerialThreadStart(slot);
  for (int i = 0; i < rounds; ++i) {
    Note(id);
    det::SerialBlock([] { return true; }, ~0ull);
  }
  det::SerialThreadExit();
}

int main() {
  REXCVAR_SET(det_frames, true);
  REXCVAR_SET(det_serial, true);
  Check(det::SerialEnabled(), "0 det_serial enabled");

  std::atomic<bool> stop{false};
  std::thread vblank([&] {
    while (!stop.load()) {
      if (det::SerialWaitIdle(200)) {
        det::NoteVblank();
        det::SerialAfterVblank();
      }
    }
  });

  // 1. Round-robin in creation order.
  constexpr int kRounds = 10;
  std::thread tb, tc;
  const int a = det::SerialRegisterThread(true);
  std::thread ta([&] {
    det::SerialThreadStart(a);
    const int b = det::SerialRegisterThread(true);  // created in A's turn: slots in creation order
    const int c = det::SerialRegisterThread(true);
    tb = std::thread(Worker, 'B', b, kRounds);
    tc = std::thread(Worker, 'C', c, kRounds);
    for (int i = 0; i < kRounds; ++i) {
      Note('A');
      det::SerialBlock([] { return true; }, ~0ull);
    }
    det::SerialThreadExit();
  });
  ta.join();
  tb.join();
  tc.join();
  std::string want;
  for (int i = 0; i < kRounds; ++i) want += "ABC";
  {
    std::lock_guard<std::mutex> lock(g_order_mu);
    Check(g_order == want, "1 round-robin in creation order (A B C x10)");
    if (g_order != want) std::printf("     got %s\n", g_order.c_str());
  }

  // 2. Deadline.
  std::atomic<uint64_t> blocked_at{0}, woke_at{0};
  const int d = det::SerialRegisterThread(true);
  std::thread td([&] {
    det::SerialThreadStart(d);
    const uint64_t v = det::Vblanks();
    blocked_at = v;
    det::SerialBlock([] { return false; }, v + 3);
    woke_at = det::Vblanks();
    det::SerialThreadExit();
  });
  td.join();
  Check(woke_at.load() == blocked_at.load() + 3, "2 a deadline wait runs at exactly V+3");

  // 3. Killed slot.
  const int e = det::SerialRegisterThread(true);
  const int f = det::SerialRegisterThread(true);
  std::atomic<bool> e_blocking{false}, f_ran{false};
  std::thread te([&] {
    det::SerialThreadStart(e);
    e_blocking = true;
    det::SerialBlock([] { return false; }, ~0ull);  // never returns: the slot is killed
    Note('e');                                      // (must not happen)
  });
  std::thread tf([&] {
    det::SerialThreadStart(f);
    det::SerialBlock([e] { return det::SerialSlotExitedLocked(e); }, ~0ull);
    f_ran = true;
    Note('F');
    det::SerialThreadExit();
  });
  WaitFor([&] { return e_blocking.load(); }, 2000ms);
  std::this_thread::sleep_for(200ms);  // a few idle vblanks: neither may run
  Check(!f_ran.load() && !det::SerialSlotExited(e), "3a a thread waiting for a live slot's exit does not run");
  det::SerialThreadKilled(e);
  Check(det::SerialSlotExited(e), "3b SerialThreadKilled: the slot reads exited");
  Check(WaitFor([&] { return f_ran.load(); }, 2000ms), "3c the waiter runs after the kill");
  tf.join();
  std::this_thread::sleep_for(100ms);
  Check(!Noted('e'), "3d the killed slot never gets the baton again");

  // 4. Drain check removal.
  std::atomic<int> calls{0};
  const int id = det::AddDrainCheck([&] {
    ++calls;
    return false;
  });
  std::atomic<bool> g_ran{false};
  const int g = det::SerialRegisterThread(true);
  std::thread tg([&] {
    det::SerialThreadStart(g);
    g_ran = true;
    det::SerialThreadExit();
  });
  std::this_thread::sleep_for(200ms);
  Check(!g_ran.load() && calls.load() > 0, "4a a check that says 'not drained' holds the next decision");
  det::RemoveDrainCheck(id);
  const int calls_at_remove = calls.load();
  Check(WaitFor([&] { return g_ran.load(); }, 2000ms), "4b after RemoveDrainCheck the decision goes on");
  tg.join();
  std::this_thread::sleep_for(100ms);
  Check(calls.load() == calls_at_remove, "4c a removed check is never called again");

  stop = true;
  vblank.join();
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
  std::fflush(stdout);
  std::_Exit(g_failures ? 1 : 0);  // (thread E stays blocked in SerialBlock: no join, no static destructors)
}
