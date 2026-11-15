// S102 lane H1: the named-thread proof. 0 game runs. Windows only.
// Starts three busy-ish threads: two that call wwe2k14::NameThread (src\thread_name.h) and one planted UNNAMED twin, and keeps the process alive
// while tools\perf\thread_busy.ps1 samples it (thread_names_test.ps1 runs both and reads the CSV). Also reads each name back itself with
// GetThreadDescription (the same call thread_busy.ps1 makes), so a name that is set wrongly fails here first.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <thread>
#include "thread_name.h"

static std::atomic<bool> g_stop{false};
static void Burn() {
  volatile unsigned x = 0;
  while (!g_stop) {
    for (int i = 0; i < 200000; ++i) x = x + i;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

static std::wstring ReadBack(std::thread& t) {
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetThreadId(t.native_handle()));
  if (!h) return L"<open failed>";
  PWSTR d = nullptr;
  std::wstring s;
  if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
    s = d;
    LocalFree(d);
  }
  CloseHandle(h);
  return s;
}

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? atoi(argv[1]) : 8;
  std::thread a([] { wwe2k14::NameThread(L"h1_test_alpha"); Burn(); });
  std::thread b([] { wwe2k14::NameThread(L"rdr_h1_test_beta"); Burn(); });
  std::thread c([] { Burn(); });  // planted: unnamed
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  int bad = 0;
  const std::wstring ra = ReadBack(a), rb = ReadBack(b), rc = ReadBack(c);
  if (ra != L"h1_test_alpha") { ++bad; std::printf("FAIL read-back alpha: '%ls'\n", ra.c_str()); }
  if (rb != L"rdr_h1_test_beta") { ++bad; std::printf("FAIL read-back beta: '%ls'\n", rb.c_str()); }
  if (!rc.empty()) { ++bad; std::printf("FAIL unnamed twin has a name: '%ls'\n", rc.c_str()); }
  std::printf("readback: alpha='%ls' beta='%ls' unnamed='%ls' -> %s\n", ra.c_str(), rb.c_str(), rc.c_str(), bad ? "FAIL" : "ok");
  std::fflush(stdout);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  g_stop = true;
  a.join(); b.join(); c.join();
  return bad ? 1 : 0;
}
