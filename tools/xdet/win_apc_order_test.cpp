// Session 77 (XDET_PLAN.md P5.2, plan review): which wins on Windows when an alertable wait finds BOTH a signalled
// object and a pending user APC: the object (WAIT_OBJECT_0) or the APC (WAIT_IO_COMPLETION)? The SDK's POSIX waits
// must give the same answer (det_serial's first probe of every guest alertable wait is a 0 ms alertable wait).
//   clang++ -O1 tools\xdet\win_apc_order_test.cpp -o out\xdet\win_apc_order_test.exe
#include <windows.h>

#include <cstdio>
#include <initializer_list>

static int g_apcs = 0;
static void CALLBACK Apc(ULONG_PTR) { ++g_apcs; }

static const char* Name(DWORD r) {
  switch (r) {
    case WAIT_OBJECT_0: return "WAIT_OBJECT_0 (object first)";
    case WAIT_OBJECT_0 + 1: return "WAIT_OBJECT_0+1";
    case WAIT_IO_COMPLETION: return "WAIT_IO_COMPLETION (APC first)";
    case WAIT_TIMEOUT: return "WAIT_TIMEOUT";
    default: return "other";
  }
}

int main() {
  for (DWORD timeout : {DWORD(0), DWORD(100)}) {
    HANDLE ev = CreateEventW(nullptr, TRUE, TRUE, nullptr);  // manual reset, signalled
    g_apcs = 0;
    QueueUserAPC(Apc, GetCurrentThread(), 0);
    DWORD r = WaitForSingleObjectEx(ev, timeout, TRUE);
    std::printf("WaitForSingleObjectEx(signalled, %lu ms, alertable) + APC pending: %s, APCs run %d\n", timeout,
                Name(r), g_apcs);
    SleepEx(0, TRUE);  // drain a leftover APC
    CloseHandle(ev);

    HANDLE a = CreateEventW(nullptr, TRUE, FALSE, nullptr), b = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    HANDLE hs[2] = {a, b};
    g_apcs = 0;
    QueueUserAPC(Apc, GetCurrentThread(), 0);
    r = WaitForMultipleObjectsEx(2, hs, FALSE, timeout, TRUE);
    std::printf("WaitForMultipleObjectsEx(any, 2nd signalled, %lu ms, alertable) + APC pending: %s, APCs run %d\n",
                timeout, Name(r), g_apcs);
    SleepEx(0, TRUE);
    CloseHandle(a), CloseHandle(b);

    HANDLE s = CreateEventW(nullptr, FALSE, FALSE, nullptr), w = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    g_apcs = 0;
    QueueUserAPC(Apc, GetCurrentThread(), 0);
    r = SignalObjectAndWait(s, w, timeout, TRUE);
    std::printf("SignalObjectAndWait(wait on signalled, %lu ms, alertable) + APC pending: %s, APCs run %d\n", timeout,
                Name(r), g_apcs);
    SleepEx(0, TRUE);
    CloseHandle(s), CloseHandle(w);
  }
  return 0;
}
