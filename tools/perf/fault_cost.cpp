// Session 49 (the RAM ties): what one write-watch fault costs a thread on this machine, outside the game.
// A page is made read-only, written (access violation), a vectored handler makes it writable again and the write is
// retried - the shape of the game's write watches (Memory::AccessViolationCallback -> unwatch -> VirtualProtect),
// without the guest memory bookkeeping. Prints the mean round trip, and the same loop's VirtualProtect calls alone.
// Build: clang++ -O2 tools\perf\fault_cost.cpp -o out\perf\fault_cost.exe ; run: out\perf\fault_cost.exe [N]
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

static char* g_page = nullptr;
static SIZE_T g_size = 0;

static LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  const char* address = reinterpret_cast<const char*>(info->ExceptionRecord->ExceptionInformation[1]);
  if (address < g_page || address >= g_page + g_size) return EXCEPTION_CONTINUE_SEARCH;
  DWORD old = 0;
  VirtualProtect(g_page, g_size, PAGE_READWRITE, &old);
  return EXCEPTION_CONTINUE_EXECUTION;
}

int main(int argc, char** argv) {
  const int n = argc > 1 ? std::atoi(argv[1]) : 20000;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  g_size = si.dwPageSize;
  g_page = static_cast<char*>(VirtualAlloc(nullptr, g_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  AddVectoredExceptionHandler(1, Handler);
  DWORD old = 0;
  volatile char* p = g_page;
  using clock = std::chrono::steady_clock;
  // Protect + fault + unprotect (in the handler) per iteration.
  auto t0 = clock::now();
  for (int i = 0; i < n; ++i) {
    VirtualProtect(g_page, g_size, PAGE_READONLY, &old);
    p[i & 63] = char(i);
  }
  auto t1 = clock::now();
  // Protect + unprotect only (the two VirtualProtect calls, no fault).
  for (int i = 0; i < n; ++i) {
    VirtualProtect(g_page, g_size, PAGE_READONLY, &old);
    VirtualProtect(g_page, g_size, PAGE_READWRITE, &old);
    p[i & 63] = char(i);
  }
  auto t2 = clock::now();
  const double fault_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / n;
  const double protect_us = std::chrono::duration<double, std::micro>(t2 - t1).count() / n;
  std::printf("iterations %d: protect + fault + unprotect %.2f us; protect + unprotect alone %.2f us; the fault "
              "itself (exception dispatch + handler entry / exit) %.2f us\n",
              n, fault_us, protect_us, fault_us - protect_us);
  return 0;
}
