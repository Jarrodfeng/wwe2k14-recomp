// Unit test for src/load_stall.h + a static check of src/load_probe.h. Build: clang++ -std=c++20 -I src tools/tests/load_stall_test.cpp
// (run from the repo root, or pass the path of src/load_probe.h as argv[1]; tools\tests\load_stall_test.ps1 does both).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include "load_stall.h"
using wwe2k14::load_probe::LastPath;
using wwe2k14::load_probe::StallOnce;
using wwe2k14::load_probe::StallWatch;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static const int64_t L = 5'000'000;
// 60 fps guest frames derived from the time: a healthy game whose UI keeps painting.
static bool U(StallWatch& w, uint32_t r, uint32_t st, int64_t now) { return w.Update(r, st, now, L, uint64_t(now / 16'667)); }
int main(int argc, char** argv) {
  StallWatch w;
  CHECK(!U(w, 0x100, 9, 1'000'000));   // first sight
  CHECK(!U(w, 0x100, 9, 1'500'000));   // keep stepping (gaps under 1 s)
  CHECK(!U(w, 0x100, 9, 2'200'000));
  CHECK(!U(w, 0x100, 9, 2'900'000));
  CHECK(!U(w, 0x100, 9, 3'600'000));
  CHECK(!U(w, 0x100, 9, 4'300'000));
  CHECK(!U(w, 0x100, 9, 5'000'000));
  CHECK(!U(w, 0x100, 9, 5'700'000));
  CHECK(!U(w, 0x100, 9, 6'000'000));   // exactly 5 s: not yet (> limit)
  CHECK(U(w, 0x100, 9, 6'000'001));    // fires
  CHECK(!U(w, 0x100, 9, 6'600'000));   // only once
  CHECK(!U(w, 0x100, 0, 7'000'000));   // 9 -> 0 resets
  CHECK(!U(w, 0x100, 9, 7'000'001));   // new 9, fresh clock
  for (int64_t t = 7'500'000; t <= 12'000'000; t += 500'000) CHECK(!U(w, 0x100, 9, t));
  CHECK(U(w, 0x100, 9, 12'000'002));   // fires again for the new stall
  StallWatch c;                        // a changing state never fires
  int64_t t = 0;
  bool any = false;
  for (int i = 0; i < 100; ++i) { t += 1'000'000; any |= U(c, 0x200, i % 2 ? 9 : 8, t); }
  CHECK(!any);
  StallWatch d;                        // a changing request in state 9 never fires
  any = false; t = 0;
  for (int i = 0; i < 100; ++i) { t += 1'000'000; any |= U(d, 0x300 + i, 9, t); }
  CHECK(!any);
  StallWatch e;                        // a long state other than 9 never fires
  CHECK(!U(e, 5, 4, 0));
  CHECK(!U(e, 5, 4, 60'000'000));
  // Always on: a worker that only ever sees (req, 9) with 2 ms steps and a painting UI fires after 5 s.
  StallWatch p;
  bool fired = false;
  t = 0;
  for (int i = 0; i < 3000; ++i) { t += 2'000; fired |= U(p, 0x400, 9, t); }  // 6 s
  CHECK(fired);
  // (a) a stopped world (sleep / debugger / freeze-thaw): a step gap over 1 s restarts the clock, no fire after the gap.
  StallWatch s;
  t = 0;
  any = false;
  for (int i = 0; i < 500; ++i) { t += 2'000; any |= U(s, 0x500, 9, t); }   // 1 s of steps
  t += 60'000'000;                                                         // the machine slept for a minute
  any |= U(s, 0x500, 9, t);
  CHECK(!any);
  for (int i = 0; i < 1000; ++i) { t += 2'000; any |= U(s, 0x500, 9, t); }  // 2 s more: still under 5 s since the thaw
  CHECK(!any);
  for (int i = 0; i < 2000; ++i) { t += 2'000; any |= U(s, 0x500, 9, t); }  // 6 s since the thaw in all: now it fires
  CHECK(any);
  // (b) the UI must keep painting: 6 s of steps with the guest frame frozen never fires; with frames moving it does.
  StallWatch fz;
  any = false;
  t = 0;
  for (int i = 0; i < 3000; ++i) { t += 2'000; any |= fz.Update(0x600, 9, t, L, 1000); }
  CHECK(!any);
  StallWatch sl;                       // 99 frames in 6 s: too few; 100: fires
  any = false;
  t = 0;
  for (int i = 0; i < 3000; ++i) { t += 2'000; any |= sl.Update(0x700, 9, t, L, 5000 + (i == 2999 ? 99 : 0)); }
  CHECK(!any);
  StallWatch sm;
  any = false;
  t = 0;
  for (int i = 0; i < 3000; ++i) { t += 2'000; any |= sm.Update(0x701, 9, t, L, 5000 + (i == 2999 ? 100 : 0)); }
  CHECK(any);
  // The stack dump is taken once per run; a second stall (new request) fires the watch again but not the dump.
  StallOnce once;
  CHECK(once.TakeDump());
  CHECK(!once.TakeDump());
  // LastPath: lock-free newest name; non-printables become '?', 16 bytes max, NUL stops.
  LastPath lp;
  CHECK(lp.Get().empty());
  const uint8_t a[20] = {'a', 'r', 'e', 'n', 'a', '.', 'p', 'a', 'c', 0};
  lp.Set(a);
  CHECK(lp.Get() == "arena.pac");
  const uint8_t b[20] = {'x', 1, 'y', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q'};
  lp.Set(b);
  CHECK(lp.Get() == "x?yabcdefghijklm");
  for (int i = 0; i < 10; ++i) lp.Set(a);  // wraps the 4 slots
  CHECK(lp.Get() == "arena.pac");
  // Static check on the real hook text: the always-on sub_82C37070 hook has no mutex / lock, its stall check sits OUTSIDE
  // the probe switch's block, takes no early return, and does not suspend host threads (samples -1); the enqueue hook's
  // probe-off path records the last file; thread creation sits in a try.
  {
    std::ifstream f(argc > 1 ? argv[1] : "src/load_probe.h");
    std::stringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();
    for (size_t r; (r = src.find('\r')) != std::string::npos;) src.erase(r, 1);  // CRLF checkouts
    const size_t a0 = src.find("REX_HOOK_RAW(sub_82C37070) {"), b0 = src.find("REX_HOOK_RAW(sub_82C37E48) {");
    CHECK(a0 != std::string::npos && b0 != std::string::npos && a0 < b0);
    const std::string hook = src.substr(a0, b0 - a0);
    CHECK(hook.find("mutex") == std::string::npos && hook.find("lock_guard") == std::string::npos &&
          hook.find("unique_lock") == std::string::npos);
    const size_t u = hook.find("g_stall.Update(");
    const size_t probe = hook.find("wwe2k14_load_probe");
    CHECK(u != std::string::npos && probe != std::string::npos && probe < u);
    CHECK(u != std::string::npos && probe != std::string::npos && hook.find("\n  }\n", probe) < u);  // probe block closed first
    CHECK(hook.find("return;") == std::string::npos);  // no early return skips the check
    CHECK(hook.find("nullptr, -1)") != std::string::npos);  // guest stacks only: no host thread is suspended
    const size_t th = hook.find("std::thread(");
    CHECK(th != std::string::npos && hook.rfind("try {", th) != std::string::npos && hook.find("catch (...)", th) != std::string::npos);
    const size_t e0 = src.find("REX_HOOK_RAW(sub_82C37E48) {");
    const size_t e1 = src.find("g_last_path.Set(", e0);
    CHECK(e1 != std::string::npos && e1 < src.find("return;", e0));  // the probe-off path records it
  }
  std::printf(fails ? "FAILED\n" : "OK\n");
  return fails;
}
