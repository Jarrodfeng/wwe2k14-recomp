#pragma once
// wwe2k14 - Session 102 lane S0-2: heat and power on the record, for the [perf][sys] line (src/perf_sys.h).
//
//   perf_pct     Windows "% Processor Performance" (all logical processors; 100 = the nominal clock, above 100 = boosting). It is the nearest
//                thing to a clock this machine offers; the old per-thread "GHz" (QueryThreadCycleTime) counted at a fixed rate and was no clock.
//   perf_limit   "% Performance Limit": the standing cap the platform applies (88 on the Surface Pro 6); a drop below its usual value = throttling.
//   limit_flags  "Performance Limit Flags": why (bit set; 2 on the Surface, meaning not yet identified).
//   pkg_w        package watts from the Energy Meter's RAPL_Package0_PKG Power counter (milliwatts there), where the machine has one.
// A counter the machine does not have prints "n/a" (never 0). A counter that reads no value in the window also reads n/a.
//
// Windows only, PDH loaded at run time (no import library, no CMake change). One query kept by the single [perf][sys] sampling thread; the first
// Collect primes it (rate / percent counters need two readings), as perf_sys::Take does.
#include <atomic>
#include <cstdint>

#if defined(_WIN32)
#include "platform/compat.h"  // <windows.h>
#include <pdh.h>
#endif

namespace wwe2k14::perf_heat {

struct Heat {
  bool probed = false;      // true when this platform tries to read heat at all (Windows); other platforms print nothing
  double perf_pct = -1;     // -1 = n/a
  double perf_limit = -1;
  double limit_flags = -1;
  double pkg_w = -1;
};

// The escape hatch (switch wwe2k14_perf_heat, default on, defined in perf_mem.h): the sampling thread stores it before every window, so it is read per
// window, never copied once. Off = the heat fields are absent from the line and no PDH call is made.
inline std::atomic<bool>& Enabled() {
  static std::atomic<bool> e{true};
  return e;
}

// What the line prints for a missing value.
inline const char* Na() { return "n/a"; }

#if defined(_WIN32)
namespace detail {
struct Api {
  HMODULE dll = nullptr;
  PDH_STATUS(WINAPI* Open)(LPCWSTR, DWORD_PTR, PDH_HQUERY*) = nullptr;
  PDH_STATUS(WINAPI* AddEnglish)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*) = nullptr;
  PDH_STATUS(WINAPI* Collect)(PDH_HQUERY) = nullptr;
  PDH_STATUS(WINAPI* GetFormatted)(PDH_HCOUNTER, DWORD, LPDWORD, PPDH_FMT_COUNTERVALUE) = nullptr;
};
struct State {
  Api api;
  PDH_HQUERY q = nullptr;
  PDH_HCOUNTER perf = nullptr, limit = nullptr, flags = nullptr, pkg = nullptr;
  bool tried = false, primed = false;
};
inline State& S() {
  static State s;
  return s;
}
inline double Read(const Api& api, PDH_HCOUNTER c) {
  if (!c) return -1;
  PDH_FMT_COUNTERVALUE v{};
  if (api.GetFormatted(c, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS) return -1;
  // CStatus 0 = PDH_CSTATUS_VALID_DATA, 1 = PDH_CSTATUS_NEW_DATA (pdhmsg.h, not included)
  if (v.CStatus != 0 && v.CStatus != 1) return -1;
  return v.doubleValue;
}
inline void Init(State& s) {
  s.tried = true;
  Api& a = s.api;
  a.dll = LoadLibraryExW(L"pdh.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);  // system folder only, never the exe's
  if (!a.dll) return;
  const auto fn = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(a.dll, n)); };
  a.Open = reinterpret_cast<decltype(a.Open)>(fn("PdhOpenQueryW"));
  a.AddEnglish = reinterpret_cast<decltype(a.AddEnglish)>(fn("PdhAddEnglishCounterW"));
  a.Collect = reinterpret_cast<decltype(a.Collect)>(fn("PdhCollectQueryData"));
  a.GetFormatted = reinterpret_cast<decltype(a.GetFormatted)>(fn("PdhGetFormattedCounterValue"));
  if (!a.Open || !a.AddEnglish || !a.Collect || !a.GetFormatted) return;
  if (a.Open(nullptr, 0, &s.q) != ERROR_SUCCESS) {
    s.q = nullptr;
    return;
  }
  // A counter this machine lacks fails here and stays null: its field then prints n/a.
  if (a.AddEnglish(s.q, L"\\Processor Information(_Total)\\% Processor Performance", 0, &s.perf) != ERROR_SUCCESS) s.perf = nullptr;
  if (a.AddEnglish(s.q, L"\\Processor Information(_Total)\\% Performance Limit", 0, &s.limit) != ERROR_SUCCESS) s.limit = nullptr;
  if (a.AddEnglish(s.q, L"\\Processor Information(_Total)\\Performance Limit Flags", 0, &s.flags) != ERROR_SUCCESS) s.flags = nullptr;
  if (a.AddEnglish(s.q, L"\\Energy Meter(RAPL_Package0_PKG)\\Power", 0, &s.pkg) != ERROR_SUCCESS) s.pkg = nullptr;
}
}  // namespace detail

// One reading over the time since the previous call. The first call primes and returns probed=true with every field n/a.
inline Heat Collect() {
  Heat h;
  if (!Enabled().load(std::memory_order_relaxed)) return h;  // probed=false: no fields, no PDH
  h.probed = true;
  detail::State& s = detail::S();
  if (!s.tried) detail::Init(s);
  if (!s.q) return h;
  if (s.api.Collect(s.q) != ERROR_SUCCESS) return h;
  if (!s.primed) {
    s.primed = true;
    return h;
  }
  h.perf_pct = detail::Read(s.api, s.perf);
  h.perf_limit = detail::Read(s.api, s.limit);
  h.limit_flags = detail::Read(s.api, s.flags);
  const double mw = detail::Read(s.api, s.pkg);
  h.pkg_w = mw >= 0 ? mw / 1000.0 : -1;  // Energy Meter Power is in milliwatts
  return h;
}
#else
inline Heat Collect() { return Heat{}; }
#endif

}  // namespace wwe2k14::perf_heat
