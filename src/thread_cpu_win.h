#pragma once
// wwe2k14 - S105 A10: the Windows side of thread_cpu.h (Toolhelp + GetThreadTimes + GetThreadDescription). Separate so the phone launcher's
// PC test build never pulls windows.h in. Included by perf_mem.h and tools\tests\thread_cpu_test.cpp. No wake count on Windows (w left out).
#if !defined(_WIN32)
#error "thread_cpu_win.h is Windows only"
#endif
#include "platform/compat.h"  // <windows.h>
#include <tlhelp32.h>

#include "thread_cpu.h"

namespace wwe2k14::thread_cpu {

inline std::string WinThreadName(DWORD id, HANDLE h) {
  using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static auto get = reinterpret_cast<GetDescFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string out;
  PWSTR w = nullptr;
  if (get && SUCCEEDED(get(h, &w)) && w) {
    char narrow[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    out = narrow;
    LocalFree(w);
  }
  if (out.empty()) out = "unnamed";
  (void)id;
  return out;
}

inline Snapshot TakeSnapshot() {
  Snapshot s;
  s.hz = 1e7;  // FILETIME: 100 ns units
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return s;
  THREADENTRY32 e = {};
  e.dwSize = sizeof(e);
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
    if (e.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.th32ThreadID);
    if (!h) continue;
    FILETIME create, exit_t, kernel, user;
    if (GetThreadTimes(h, &create, &exit_t, &kernel, &user)) {
      auto u64 = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
      Raw r;
      r.name = WinThreadName(e.th32ThreadID, h);
      r.ticks = u64(kernel) + u64(user);
      s.t[int32_t(e.th32ThreadID)] = std::move(r);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  return s;
}

inline bool StepNow(Meter& m, double t_log, Window& out) { return m.Step(TakeSnapshot(), SteadySeconds(), t_log, out); }

}  // namespace wwe2k14::thread_cpu
